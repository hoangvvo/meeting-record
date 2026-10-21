/*
 * Windows backend: WASAPI application (process) loopback.
 *
 * Unlike render-endpoint loopback, process loopback captures a specific process
 * tree and is unaffected by the output device, volume, or mute state.
 *
 * Requires Windows 10 build 19041 / Windows 11. No permission prompt exists; API
 * availability is the only gate.
 *
 * Constraints:
 *   - activation is asynchronous via a completion handler
 *   - the client must be initialised in shared mode with an explicit format;
 *     GetMixFormat is unavailable on a loopback-activated client
 *   - INCLUDE_PROCESS_TREE also captures child processes, which is required for
 *     Chrome, Electron and Teams
 */
#include "meeting-record.h"
#include "meeting-record-detect.h"
#include "AudioAppProcess.h"
#include "TimelineMixer.h"
#include "meeting-catalog.h"
#include "tracks.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <appmodel.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <psapi.h>
#include <tlhelp32.h>

using Microsoft::WRL::ComPtr;

namespace {

using meeting_record::kSampleRate;
using meeting_record::TimelineMixer;

/*
 * Capacity, not latency: polling drains whatever is ready. A second rides out
 * scheduling stalls that a 5 ms sleep at the default timer resolution invites.
 */
constexpr REFERENCE_TIME kBufferDuration = 10000000; /* 100-nanosecond units */

std::mutex g_mutex;
std::string g_last_error = "no error";

void SetLastErrorMessage(const std::string &message) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_error = message;
}

std::string LastErrorMessage() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_last_error;
}

std::string HResultMessage(const char *what, HRESULT hr) {
  char buf[192];
  std::snprintf(buf, sizeof buf, "%s failed: 0x%08lX", what,
                static_cast<unsigned long>(hr));
  return buf;
}

class ScopedComApartment {
public:
  ScopedComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ScopedComApartment() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }

private:
  HRESULT result_;
};

/* Keeps the MTA alive while clients exist: drain threads leave COM before Stop(). */
class ScopedMtaUsage {
public:
  ScopedMtaUsage() : result_(CoIncrementMTAUsage(&cookie_)) {}
  ~ScopedMtaUsage() {
    if (SUCCEEDED(result_)) CoDecrementMTAUsage(cookie_);
  }
  ScopedMtaUsage(const ScopedMtaUsage &) = delete;
  ScopedMtaUsage &operator=(const ScopedMtaUsage &) = delete;

  HRESULT result() const { return result_; }

private:
  CO_MTA_USAGE_COOKIE cookie_ = nullptr;
  HRESULT result_;
};

/* What Settings > Privacy & security > Microphone records for `key`. */
bool ConsentDenied(HKEY root, const std::wstring &key) {
  wchar_t value[16] = {};
  DWORD size = sizeof value;
  return RegGetValueW(root, key.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, value,
                      &size) == ERROR_SUCCESS &&
         _wcsicmp(value, L"Deny") == 0;
}

struct AudioActivity {
  bool rendering = false;
  bool capturing = false;
};

std::map<DWORD, AudioActivity> CollectAudioActivity() {
  ScopedComApartment apartment;
  std::map<DWORD, AudioActivity> activity;
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return activity;
  }

  for (EDataFlow flow : {eRender, eCapture}) {
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              devices.GetAddressOf()))) {
      continue;
    }
    UINT device_count = 0;
    devices->GetCount(&device_count);
    for (UINT index = 0; index < device_count; index++) {
      ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(index, device.GetAddressOf()))) continue;
      ComPtr<IAudioSessionManager2> manager;
      if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void **>(
                                               manager.GetAddressOf())))) {
        continue;
      }
      ComPtr<IAudioSessionEnumerator> sessions;
      if (FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf()))) continue;
      int session_count = 0;
      sessions->GetCount(&session_count);
      for (int session_index = 0; session_index < session_count; session_index++) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(session_index, control.GetAddressOf()))) continue;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(&control2))) continue;
        DWORD pid = 0;
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(control2->GetProcessId(&pid)) || pid == 0 ||
            FAILED(control->GetState(&state)) || state != AudioSessionStateActive) {
          continue;
        }
        if (flow == eRender) {
          activity[pid].rendering = true;
        } else {
          activity[pid].capturing = true;
        }
      }
    }
  }
  return activity;
}

/* ActivateAudioInterfaceAsync reports through a handler, so wrap a waitable event. */
class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
  ActivationHandler() {
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~ActivationHandler() {
    if (event_) CloseHandle(event_);
  }

  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
    HRESULT activate_hr = S_OK;
    ComPtr<IUnknown> punk;
    HRESULT hr = op->GetActivateResult(&activate_hr, &punk);
    result_ = SUCCEEDED(hr) ? activate_hr : hr;
    if (SUCCEEDED(result_)) punk.As(&client_);
    SetEvent(event_);
    return S_OK;
  }

  /* The handler runs on an MTA thread pool thread, so a plain wait is safe. */
  HRESULT Wait(DWORD timeout_ms) {
    if (!event_) return E_FAIL;
    return WaitForSingleObject(event_, timeout_ms) == WAIT_OBJECT_0 ? result_
                                                                   : E_FAIL;
  }

  ComPtr<IAudioClient> client() const { return client_; }

private:
  HANDLE event_ = nullptr;
  HRESULT result_ = E_FAIL;
  ComPtr<IAudioClient> client_;
};

/* One activated loopback client plus the thread draining it. */
struct LoopbackStream {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  uint32_t pid = 0;
  bool exclude = false;
  uint32_t channels = 0;
};

struct MicrophoneStream {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  std::wstring device_id;
  std::string name;
};

/* What a WASAPI failure means for its track. */
mrec_track_reason ReasonFor(HRESULT hr) {
  switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED: return MREC_TRACK_REASON_DEVICE_REMOVED;
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return MREC_TRACK_REASON_SERVICE_RESET;
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return MREC_TRACK_REASON_FORMAT_CHANGED;
    default: return MREC_TRACK_REASON_UNKNOWN;
  }
}

/* The name Windows shows for `device`, such as "Headset Microphone (Jabra)". */
std::string FriendlyName(IMMDevice *device) {
  /* PKEY_Device_FriendlyName, spelled out to avoid another import library. */
  static const PROPERTYKEY kFriendlyName = {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name;
  if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR &&
      value.pwszVal) {
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      name.resize(static_cast<size_t>(size));
      WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name.data(), size, nullptr, nullptr);
      name.resize(static_cast<size_t>(size) - 1);
    }
  }
  PropVariantClear(&value);
  return name;
}

struct ProcessEntry {
  uint32_t parent = 0;
  int32_t platform = MREC_PLATFORM_UNKNOWN;
};

/* Every process's parent, and the conferencing app its executable belongs to. */
std::map<uint32_t, ProcessEntry> ProcessSnapshot() {
  std::map<uint32_t, ProcessEntry> processes;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return processes;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      char executable[MAX_PATH] = {};
      WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, executable, sizeof executable,
                          nullptr, nullptr);
      processes[entry.th32ProcessID] = {entry.th32ParentProcessID,
                                        mrec_catalog_executable_platform(executable)};
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return processes;
}

/* Whether `pid` is one of `roots` or descends from one. */
bool InTrees(const std::map<uint32_t, ProcessEntry> &processes, uint32_t pid,
             const std::vector<uint32_t> &roots) {
  for (size_t remaining = processes.size() + 1; remaining > 0; remaining--) {
    if (std::find(roots.begin(), roots.end(), pid) != roots.end()) return true;
    const auto process = processes.find(pid);
    if (process == processes.end() || process->second.parent == pid) return false;
    pid = process->second.parent;
  }
  return false;
}

std::wstring DeviceId(IMMDevice *device) {
  LPWSTR raw_id = nullptr;
  if (FAILED(device->GetId(&raw_id)) || raw_id == nullptr) return {};
  std::wstring id(raw_id);
  CoTaskMemFree(raw_id);
  return id;
}

/* Active, then inactive, sessions on `device` from processes in `roots`' trees. */
std::pair<int, int> CapturedSessions(IMMDevice *device,
                                     const std::map<uint32_t, ProcessEntry> &processes,
                                     const std::vector<uint32_t> &roots) {
  std::pair<int, int> counts{0, 0};
  ComPtr<IAudioSessionManager2> manager;
  if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(manager.GetAddressOf())))) {
    return counts;
  }
  ComPtr<IAudioSessionEnumerator> sessions;
  if (FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf()))) return counts;
  int session_count = 0;
  sessions->GetCount(&session_count);
  for (int index = 0; index < session_count; index++) {
    ComPtr<IAudioSessionControl> control;
    if (FAILED(sessions->GetSession(index, control.GetAddressOf()))) continue;
    ComPtr<IAudioSessionControl2> control2;
    if (FAILED(control.As(&control2))) continue;
    DWORD pid = 0;
    AudioSessionState state = AudioSessionStateExpired;
    if (FAILED(control2->GetProcessId(&pid)) || pid == 0 ||
        FAILED(control->GetState(&state)) || state == AudioSessionStateExpired ||
        !InTrees(processes, pid, roots)) {
      continue;
    }
    (state == AudioSessionStateActive ? counts.first : counts.second)++;
  }
  return counts;
}

/*
 * The microphone the captured apps record from, as Recall picks it: the capture
 * device holding most of their sessions. Otherwise the default communications
 * microphone, which calls use, as Granola picks it.
 */
std::optional<std::wstring> PreferredMicrophoneId(const std::vector<uint32_t> &targets) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return std::nullopt;
  }
  ComPtr<IMMDeviceCollection> devices;
  if (!targets.empty() && SUCCEEDED(enumerator->EnumAudioEndpoints(
                              eCapture, DEVICE_STATE_ACTIVE, devices.GetAddressOf()))) {
    const auto processes = ProcessSnapshot();
    std::wstring best;
    std::pair<int, int> best_counts{0, 0};
    UINT device_count = 0;
    devices->GetCount(&device_count);
    for (UINT index = 0; index < device_count; index++) {
      ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(index, device.GetAddressOf()))) continue;
      const auto counts = CapturedSessions(device.Get(), processes, targets);
      if (counts > best_counts) {
        best_counts = counts;
        best = DeviceId(device.Get());
      }
    }
    if (!best.empty()) return best;
  }
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device))) {
    return std::nullopt;
  }
  std::wstring id = DeviceId(device.Get());
  if (id.empty()) return std::nullopt;
  return id;
}

uint64_t MonotonicTimeNs() {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
      !QueryPerformanceCounter(&counter)) {
    return GetTickCount64() * 1000000ull;
  }
  const uint64_t whole = static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart);
  const uint64_t remainder = static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart);
  return whole * 1000000000ull +
         remainder * 1000000000ull / static_cast<uint64_t>(frequency.QuadPart);
}

/* Loopback activation succeeds for any pid, so a dead target would record silence. */
bool IsProcessRunning(uint32_t pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
  const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
}

std::optional<std::wstring> PackageFamilyName(uint32_t pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return std::nullopt;
  wchar_t name[256];
  UINT32 length = static_cast<UINT32>(std::size(name));
  const LONG result = GetPackageFamilyName(process, &length, name);
  CloseHandle(process);
  if (result != ERROR_SUCCESS) return std::nullopt;
  return std::wstring(name);
}

bool InSession(uint32_t pid, DWORD session) {
  DWORD process_session = 0;
  return ProcessIdToSessionId(pid, &process_session) && process_session == session;
}

/*
 * The process trees to capture for `pids`. A conferencing app brings its sibling
 * processes, and a packaged app the rest of its package: Windows may start them
 * outside the app's tree, as new Teams does with its call container. A process
 * inside another target's tree is dropped, since a second client would double
 * its audio.
 */
std::vector<uint32_t> CaptureRoots(std::vector<uint32_t> pids) {
  const auto processes = ProcessSnapshot();
  if (processes.empty()) return pids;

  std::vector<int32_t> apps;
  std::vector<std::wstring> packages;
  for (uint32_t pid : pids) {
    const auto process = processes.find(pid);
    if (process != processes.end() && process->second.platform != MREC_PLATFORM_UNKNOWN) {
      apps.push_back(process->second.platform);
    }
    if (auto package = PackageFamilyName(pid)) packages.push_back(std::move(*package));
  }
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  for (const auto &[pid, process] : processes) {
    bool sibling = process.platform != MREC_PLATFORM_UNKNOWN &&
                   std::find(apps.begin(), apps.end(), process.platform) != apps.end();
    if (!sibling && !packages.empty()) {
      const auto package = PackageFamilyName(pid);
      sibling = package && std::find(packages.begin(), packages.end(), *package) != packages.end();
    }
    if (sibling && InSession(pid, session)) pids.push_back(pid);
  }
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());

  const auto inside_another = [&](uint32_t pid) {
    uint32_t current = pid;
    for (size_t remaining = processes.size(); remaining > 0; remaining--) {
      const auto process = processes.find(current);
      if (process == processes.end() || process->second.parent == current ||
          process->second.parent == pid) {
        return false;
      }
      current = process->second.parent;
      if (std::binary_search(pids.begin(), pids.end(), current)) return true;
    }
    return false;
  };
  std::vector<uint32_t> roots;
  for (uint32_t pid : pids) {
    if (!inside_another(pid)) roots.push_back(pid);
  }
  return roots;
}

bool SupportsProcessLoopback() {
  using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return true;
  auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtl_get_version) return true;

  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof version;
  if (rtl_get_version(&version) != 0) return true;
  return version.dwMajorVersion > 10 ||
         (version.dwMajorVersion == 10 && version.dwBuildNumber >= 19041);
}

class Session {
public:
  static Session &Instance() {
    static Session session;
    return session;
  }

  mrec_status Start(const uint32_t *pids, size_t pid_count, int32_t global_mixdown,
                    int32_t mono, int32_t microphone,
                    mrec_audio_callback system_callback,
                    void *system_user_data,
                    mrec_audio_callback microphone_callback,
                    void *microphone_user_data,
                    mrec_track_event_callback event_callback,
                    void *event_user_data) {
    ScopedComApartment apartment;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!SupportsProcessLoopback()) {
      SetLastErrorMessage("WASAPI process loopback requires Windows build 19041 or newer");
      return MREC_ERR_UNSUPPORTED_OS;
    }
    if (running_) {
      SetLastErrorMessage("capture already running");
      return MREC_ERR_ALREADY_RUNNING;
    }

    std::vector<uint32_t> targets;
    if (pids && pid_count > 0) targets.assign(pids, pids + pid_count);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.empty() && !global_mixdown) {
      SetLastErrorMessage("no pids supplied and global_mixdown not set");
      return MREC_ERR_NO_PROCESSES;
    }
    if (!global_mixdown) {
      targets.erase(std::remove_if(targets.begin(), targets.end(),
                                   [](uint32_t pid) { return !IsProcessRunning(pid); }),
                    targets.end());
      if (targets.empty()) {
        SetLastErrorMessage("none of the requested pids are running");
        return MREC_ERR_NO_PROCESSES;
      }
      targets = CaptureRoots(std::move(targets));
    }

    if (microphone != MREC_MICROPHONE_NONE &&
        microphone != MREC_MICROPHONE_DEFAULT) {
      SetLastErrorMessage("unknown microphone source");
      return MREC_ERR_INTERNAL;
    }

    mta_usage_.emplace();
    if (FAILED(mta_usage_->result())) {
      SetLastErrorMessage(HResultMessage("CoIncrementMTAUsage", mta_usage_->result()));
      mta_usage_.reset();
      return MREC_ERR_INTERNAL;
    }

    callback_ = system_callback;
    user_data_ = system_user_data;
    microphone_callback_ = microphone_callback;
    microphone_user_data_ = microphone_user_data;
    channels_ = mono ? 1 : 2;

    /* One client per target process; EXCLUDE semantics produces a system-wide tap. */
    if (global_mixdown) {
      /* WASAPI's system-wide form is "everything except this process tree" and
       * still requires a real target pid. PID 0 is rejected on supported Windows
       * builds. Excluding the recorder also prevents feedback from monitoring UI
       * sounds. */
      auto stream = Activate(GetCurrentProcessId(), /*exclude=*/true);
      if (!stream) {
        mta_usage_.reset();
        return MREC_ERR_TAP_FAILED;
      }
      streams_.push_back(std::move(*stream));
    } else {
      for (uint32_t pid : targets) {
        auto stream = Activate(pid, /*exclude=*/false);
        if (!stream) {
          /* One dead or unsupported process should not sink the session. */
          continue;
        }
        streams_.push_back(std::move(*stream));
      }
      if (streams_.empty()) {
        SetLastErrorMessage("could not create a loopback client for any pid");
        mta_usage_.reset();
        return MREC_ERR_TAP_FAILED;
      }
    }

    if (!global_mixdown) targets_ = targets;
    if (microphone == MREC_MICROPHONE_DEFAULT) {
      mrec_status failure = MREC_ERR_DEVICE_FAILED;
      auto stream = ActivateMicrophone(PreferredMicrophoneId(targets_), failure);
      if (!stream) {
        streams_.clear();
        targets_.clear();
        mta_usage_.reset();
        return failure;
      }
      microphone_ = std::move(*stream);
      microphone_enabled_ = true;
    }

    system_mixer_.Reset(streams_.size(), channels_, callback_, user_data_);
    if (microphone_) {
      microphone_mixer_.Reset(1, 1, microphone_callback_, microphone_user_data_);
    }
    running_ = true;
    stop_requested_ = false;
    mrec_tracks_open(event_callback, event_user_data);
    mrec_track_started(MREC_TRACK_SYSTEM_AUDIO, nullptr);
    if (microphone_) mrec_track_started(MREC_TRACK_MICROPHONE, microphone_->name.c_str());
    for (size_t i = 0; i < streams_.size(); i++) {
      HRESULT hr = streams_[i].client->Start();
      if (FAILED(hr)) {
        SetLastErrorMessage(HResultMessage("IAudioClient::Start", hr));
        StopLocked();
        return MREC_ERR_IOPROC_FAILED;
      }
      workers_.emplace_back([this, i] { Drain(i); });
    }
    if (microphone_) {
      HRESULT hr = microphone_->client->Start();
      if (FAILED(hr)) {
        SetLastErrorMessage(HResultMessage("microphone IAudioClient::Start", hr));
        StopLocked();
        return MREC_ERR_IOPROC_FAILED;
      }
      microphone_worker_ = std::thread([this] { DrainMicrophone(); });
    }
    SetLastErrorMessage("no error");
    return MREC_OK;
  }

  mrec_status Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return MREC_ERR_NOT_RUNNING;
    StopLocked();
    return MREC_OK;
  }

  bool running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  void Format(double *rate, uint32_t *channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate) *rate = kSampleRate;
    if (channels) *channels = channels_;
  }

  bool MicrophoneFormat(double *rate, uint32_t *channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !microphone_enabled_) return false;
    if (rate) *rate = kSampleRate;
    if (channels) *channels = 1;
    return true;
  }

private:
  std::optional<LoopbackStream> Activate(uint32_t pid, bool exclude) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    /* Chrome, Electron and Teams render audio from helpers, not the visible pid. */
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activate_params{};
    activate_params.vt = VT_BLOB;
    activate_params.blob.cbSize = sizeof params;
    activate_params.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

    ComPtr<ActivationHandler> handler = Microsoft::WRL::Make<ActivationHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &activate_params, handler.Get(), &op);
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("ActivateAudioInterfaceAsync", hr));
      return std::nullopt;
    }

    hr = handler->Wait(5000);
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("loopback activation", hr));
      return std::nullopt;
    }

    LoopbackStream stream;
    stream.pid = pid;
    stream.exclude = exclude;
    stream.client = handler->client();
    if (!stream.client) {
      SetLastErrorMessage("activation returned no IAudioClient");
      return std::nullopt;
    }

    /*
     * A loopback-activated client cannot report a mix format. Float32 at 48 kHz
     * matches the macOS backend.
     */
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(channels_);
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    /*
     * Polling mode, so no AUDCLNT_STREAMFLAGS_EVENTCALLBACK: that flag without a
     * following SetEventHandle() makes Start() fail with
     * AUDCLNT_E_EVENTHANDLE_NOT_SET.
     */
    hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK |
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   kBufferDuration, 0, &format, nullptr);
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("IAudioClient::Initialize", hr));
      return std::nullopt;
    }

    hr = stream.client->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void **>(
                                       stream.capture.GetAddressOf()));
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("GetService(IAudioCaptureClient)", hr));
      return std::nullopt;
    }
    stream.channels = channels_;
    return stream;
  }

  /* `failure` receives the status to report; messages match the macOS backend. */
  std::optional<MicrophoneStream> ActivateMicrophone(
      const std::optional<std::wstring> &device_id, mrec_status &failure) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto fail = [&](const char *what, HRESULT hr) {
      if (SUCCEEDED(initialized)) CoUninitialize();
      if (hr == E_ACCESSDENIED) {
        /* Settings > Privacy & security > Microphone denies desktop apps. */
        failure = MREC_ERR_PERMISSION;
        SetLastErrorMessage("microphone permission is not granted");
      } else {
        failure = MREC_ERR_DEVICE_FAILED;
        SetLastErrorMessage(hr == E_NOTFOUND
                                ? "microphone setup failed: no default microphone is available"
                                : "microphone setup failed: " + HResultMessage(what, hr));
      }
      return std::nullopt;
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return fail("create audio device enumerator", hr);

    if (!device_id) return fail("get default microphone", E_NOTFOUND);
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDevice(device_id->c_str(), &device);
    if (FAILED(hr)) return fail("get microphone", hr);

    MicrophoneStream stream;
    stream.device_id = *device_id;
    stream.name = FriendlyName(device.Get());
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(stream.client.GetAddressOf()));
    if (FAILED(hr)) return fail("activate microphone", hr);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    hr = stream.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        kBufferDuration, 0, &format, nullptr);
    if (FAILED(hr)) return fail("initialize microphone", hr);

    hr = stream.client->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void **>(stream.capture.GetAddressOf()));
    if (FAILED(hr)) return fail("get microphone capture client", hr);

    /* A stream on the communications microphone would otherwise duck other audio. */
    ComPtr<IAudioSessionControl> control;
    ComPtr<IAudioSessionControl2> control2;
    if (SUCCEEDED(stream.client->GetService(IID_PPV_ARGS(&control))) &&
        SUCCEEDED(control.As(&control2))) {
      control2->SetDuckingPreference(TRUE);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return stream;
  }

  void Drain(size_t index) {
    /* Each drain thread needs its own COM apartment. */
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LoopbackStream &stream = streams_[index];
    uint64_t next_exit_check = 0;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
      /* Exited targets deliver silence; say so, and keep recording. */
      if (index == 0 && !targets_.empty() && next_exit_check != UINT64_MAX) {
        const uint64_t now = MonotonicTimeNs();
        if (now >= next_exit_check) {
          next_exit_check = now + 1000000000ull;
          if (std::none_of(targets_.begin(), targets_.end(), IsProcessRunning)) {
            next_exit_check = UINT64_MAX;
            mrec_track_interrupted(MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
                                   MREC_TRACK_REASON_APP_EXITED, "the captured apps exited");
          }
        }
      }
      UINT32 packet = 0;
      HRESULT hr = stream.capture->GetNextPacketSize(&packet);
      if (FAILED(hr)) {
        RecoverLoopback(index, ReasonFor(hr), HResultMessage("capture", hr));
        continue;
      }
      if (packet == 0) {
        Sleep(5);
        continue;
      }
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0, qpc = 0;
      hr = stream.capture->GetBuffer(&data, &frames, &flags, &position, &qpc);
      if (FAILED(hr)) {
        RecoverLoopback(index, ReasonFor(hr), HResultMessage("capture", hr));
        continue;
      }
      if (frames > 0) {
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        const uint64_t timestamp =
            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || qpc == 0
                ? MonotonicTimeNs()
                : qpc * 100;
        system_mixer_.Add(index, reinterpret_cast<const float *>(data), frames,
                          silent, discontinuity, timestamp);
      }
      stream.capture->ReleaseBuffer(frames);
    }
    CoUninitialize();
  }

  void DrainMicrophone() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint64_t next_device_check = 0;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
      if (!microphone_) break;
      const uint64_t now = MonotonicTimeNs();
      if (now >= next_device_check) {
        next_device_check = now + 500000000ull;
        const auto preferred = PreferredMicrophoneId(targets_);
        if (!preferred) {
          RecoverMicrophone(MREC_TRACK_REASON_DEVICE_REMOVED, "no microphone is available");
          continue;
        }
        if (preferred.value() != microphone_->device_id) {
          RecoverMicrophone(MREC_TRACK_REASON_DEVICE_CHANGED, "the microphone in use changed");
          continue;
        }
      }
      UINT32 packet = 0;
      HRESULT hr = microphone_->capture->GetNextPacketSize(&packet);
      if (FAILED(hr)) {
        RecoverMicrophone(ReasonFor(hr), HResultMessage("capture", hr));
        continue;
      }
      if (packet == 0) {
        Sleep(5);
        continue;
      }
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0, qpc = 0;
      hr = microphone_->capture->GetBuffer(&data, &frames, &flags, &position, &qpc);
      if (FAILED(hr)) {
        RecoverMicrophone(ReasonFor(hr), HResultMessage("capture", hr));
        continue;
      }
      if (frames > 0) {
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        const uint64_t timestamp =
            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || qpc == 0
                ? MonotonicTimeNs()
                : qpc * 100;
        microphone_mixer_.Add(0, reinterpret_cast<const float *>(data), frames,
                              silent, discontinuity, timestamp);
      }
      microphone_->capture->ReleaseBuffer(frames);
    }
    CoUninitialize();
  }

  /* What a recovery needs through the shared C callbacks. */
  struct Recovery {
    Session *session;
    size_t index;
  };

  static int32_t WaitForRetry(void *context, uint32_t milliseconds) {
    const Session *session = static_cast<Recovery *>(context)->session;
    for (uint32_t waited = 0; waited < milliseconds; waited += 50) {
      if (session->stop_requested_.load(std::memory_order_relaxed)) return 0;
      Sleep((std::min)(50u, milliseconds - waited));
    }
    return session->stop_requested_.load(std::memory_order_relaxed) ? 0 : 1;
  }

  static int32_t ReopenLoopback(void *context, char *, size_t) {
    auto *recovery = static_cast<Recovery *>(context);
    LoopbackStream &stream = recovery->session->streams_[recovery->index];
    auto replacement = recovery->session->Activate(stream.pid, stream.exclude);
    if (!replacement) return 0;
    HRESULT hr = replacement->client->Start();
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("IAudioClient::Start", hr));
      return 0;
    }
    stream = std::move(*replacement);
    return 1;
  }

  static int32_t ReopenMicrophone(void *context, char *device_name, size_t capacity) {
    Session *session = static_cast<Recovery *>(context)->session;
    mrec_status failure = MREC_ERR_DEVICE_FAILED;
    auto replacement =
        session->ActivateMicrophone(PreferredMicrophoneId(session->targets_), failure);
    if (!replacement) return 0;
    HRESULT hr = replacement->client->Start();
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("microphone IAudioClient::Start", hr));
      return 0;
    }
    session->microphone_ = std::move(*replacement);
    std::snprintf(device_name, capacity, "%s", session->microphone_->name.c_str());
    return 1;
  }

  /* Never stops the recording: retries until the source returns or Stop(). */
  void RecoverLoopback(size_t index, mrec_track_reason reason, const std::string &detail) {
    if (streams_[index].client) streams_[index].client->Stop();
    const std::string message = "system audio interrupted: " + detail;
    SetLastErrorMessage(message);
    Recovery recovery{this, index};
    mrec_track_recover(MREC_TRACK_SYSTEM_AUDIO, static_cast<uint32_t>(index), reason,
                       message.c_str(), ReopenLoopback, WaitForRetry, &recovery);
  }

  void RecoverMicrophone(mrec_track_reason reason, const std::string &detail) {
    if (microphone_ && microphone_->client) microphone_->client->Stop();
    const std::string message = "microphone interrupted: " + detail;
    SetLastErrorMessage(message);
    Recovery recovery{this, 0};
    mrec_track_recover(MREC_TRACK_MICROPHONE, 0, reason, message.c_str(), ReopenMicrophone,
                       WaitForRetry, &recovery);
  }

  void StopLocked() {
    stop_requested_ = true;
    for (auto &worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
    if (microphone_worker_.joinable()) microphone_worker_.join();
    system_mixer_.Flush();
    if (microphone_) microphone_mixer_.Flush();
    for (auto &stream : streams_) {
      if (stream.client) stream.client->Stop();
    }
    streams_.clear();
    if (microphone_ && microphone_->client) microphone_->client->Stop();
    microphone_.reset();
    targets_.clear();
    mta_usage_.reset();
    microphone_enabled_ = false;
    callback_ = nullptr;
    user_data_ = nullptr;
    microphone_callback_ = nullptr;
    microphone_user_data_ = nullptr;
    running_ = false;
    mrec_tracks_close();
  }

  std::mutex mutex_;
  bool running_ = false;
  std::optional<ScopedMtaUsage> mta_usage_;
  std::atomic<bool> stop_requested_{false};
  /* Capture roots, which also pick the microphone; empty for system capture. */
  std::vector<uint32_t> targets_;
  std::vector<LoopbackStream> streams_;
  std::vector<std::thread> workers_;
  std::optional<MicrophoneStream> microphone_;
  bool microphone_enabled_ = false;
  std::thread microphone_worker_;
  TimelineMixer system_mixer_;
  TimelineMixer microphone_mixer_;
  mrec_audio_callback callback_ = nullptr;
  void *user_data_ = nullptr;
  mrec_audio_callback microphone_callback_ = nullptr;
  void *microphone_user_data_ = nullptr;
  uint32_t channels_ = 1;
};

} // namespace

extern "C" {

mrec_permission mrec_audio_permission_status(void) {
  /*
   * No TCC analogue exists. The OS version gate surfaces as an activation failure
   * from mrec_start rather than as a permission state.
   */
  return MREC_PERM_GRANTED;
}

mrec_status mrec_request_audio_permission(void) { return MREC_OK; }

mrec_permission mrec_microphone_permission_status(void) {
  /* The device, the user, then this app: packaged apps have their own entry. */
  const std::wstring store = L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                             L"CapabilityAccessManager\\ConsentStore\\microphone";
  std::wstring app = store + L"\\NonPackaged";
  UINT32 length = 0;
  if (GetCurrentPackageFamilyName(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
    std::wstring family(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, family.data()) == ERROR_SUCCESS) {
      family.resize(length - 1);
      app = store + L"\\" + family;
    }
  }
  const bool denied = ConsentDenied(HKEY_LOCAL_MACHINE, store) ||
                      ConsentDenied(HKEY_CURRENT_USER, store) ||
                      ConsentDenied(HKEY_CURRENT_USER, app);
  return denied ? MREC_PERM_DENIED : MREC_PERM_GRANTED;
}

mrec_status mrec_request_microphone_permission(void) { return MREC_OK; }

mrec_status mrec_list_audio_processes(mrec_process *out, size_t capacity,
                                          size_t *out_count) {
  if (!out || !out_count) return MREC_ERR_INTERNAL;
  const auto activity = CollectAudioActivity();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return MREC_ERR_INTERNAL;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  size_t count = 0;

  if (Process32FirstW(snapshot, &entry)) {
    do {
      const auto found = activity.find(entry.th32ProcessID);
      if (found == activity.end()) continue;
      if (count >= capacity) continue;
      mrec_process &slot = out[count++];
      std::memset(&slot, 0, sizeof slot);
      slot.pid = entry.th32ProcessID;
      slot.is_running_output = found->second.rendering ? 1 : 0;
      slot.is_running_input = found->second.capturing ? 1 : 0;
      WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, slot.name,
                          sizeof slot.name, nullptr, nullptr);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  *out_count = count;
  return activity.size() > capacity ? MREC_ERR_BUFFER_TOO_SMALL : MREC_OK;
}

mrec_status mrec_list_audio_apps(mrec_process *out, size_t capacity,
                                     size_t *out_count) {
  if (!out || !out_count) return MREC_ERR_INTERNAL;
  *out_count = 0;
  const auto activity = CollectAudioActivity();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return MREC_ERR_INTERNAL;

  std::map<uint32_t, meeting_record::AudioAppProcess> processes;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      processes.emplace(entry.th32ProcessID, meeting_record::AudioAppProcess{
          entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  EnumWindows([](HWND window, LPARAM context) -> BOOL {
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
      return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    auto &processes = *reinterpret_cast<
        std::map<uint32_t, meeting_record::AudioAppProcess> *>(context);
    const auto process = processes.find(pid);
    if (process != processes.end()) process->second.has_app_window = true;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&processes));

  DWORD shell_pid = 0;
  GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
  std::map<uint32_t, mrec_process> apps;
  for (const auto &[pid, state] : activity) {
    const auto *root = meeting_record::FindAudioAppRoot(processes, pid, shell_pid);
    if (!root || root->pid == GetCurrentProcessId()) continue;
    auto &app = apps[root->pid];
    app.pid = root->pid;
    app.is_running_output |= state.rendering ? 1 : 0;
    app.is_running_input |= state.capturing ? 1 : 0;
    WideCharToMultiByte(CP_UTF8, 0, root->executable.c_str(), -1, app.name,
                        sizeof app.name, nullptr, nullptr);
  }

  size_t count = 0;
  for (const auto &[pid, app] : apps) {
    if (count == capacity) break;
    out[count++] = app;
  }
  *out_count = count;
  return apps.size() > capacity ? MREC_ERR_BUFFER_TOO_SMALL : MREC_OK;
}

int32_t mrec_start_raw(const uint32_t *pids, size_t pid_count,
                         int32_t global_mixdown, int32_t mono,
                         int32_t mute_captured_output,
                         mrec_audio_callback cb, void *user_data) {
  return mrec_start_tracks_raw(
      pids, pid_count, global_mixdown, mono, mute_captured_output,
      MREC_MICROPHONE_NONE, cb, user_data, nullptr, nullptr, nullptr, nullptr);
}

int32_t mrec_start_tracks_raw(
    const uint32_t *pids, size_t pid_count, int32_t global_mixdown, int32_t mono,
    int32_t mute_captured_output, int32_t microphone,
    mrec_audio_callback system_audio_cb, void *system_audio_user_data,
    mrec_audio_callback microphone_cb, void *microphone_user_data,
    mrec_track_event_callback event_cb, void *event_user_data) {
  /* Process loopback cannot mute the process it captures. */
  (void)mute_captured_output;
  return Session::Instance().Start(
      pids, pid_count, global_mixdown, mono, microphone, system_audio_cb,
      system_audio_user_data, microphone_cb, microphone_user_data, event_cb,
      event_user_data);
}

mrec_status mrec_stop(void) { return Session::Instance().Stop(); }

int32_t mrec_is_running(void) { return Session::Instance().running() ? 1 : 0; }

mrec_status mrec_current_format(double *sample_rate, uint32_t *channels) {
  if (!Session::Instance().running()) return MREC_ERR_NOT_RUNNING;
  Session::Instance().Format(sample_rate, channels);
  return MREC_OK;
}

mrec_status mrec_current_microphone_format(double *sample_rate,
                                           uint32_t *channels) {
  return Session::Instance().MicrophoneFormat(sample_rate, channels)
             ? MREC_OK
             : MREC_ERR_NOT_RUNNING;
}

const char *mrec_last_error(void) {
  thread_local std::string snapshot;
  std::lock_guard<std::mutex> lock(g_mutex);
  snapshot = g_last_error;
  return snapshot.c_str();
}

} // extern "C"
