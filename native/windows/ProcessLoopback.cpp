/*
 * Windows backend: WASAPI application (process) loopback.
 *
 * Unlike render-endpoint loopback, process loopback captures a specific process
 * tree and is unaffected by the output device, volume, or mute state.
 *
 * Requires Windows 10 build 20348 / Windows 11. No permission prompt exists; API
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

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFramesPerBlock = 480; /* 10 ms at 48 kHz. */
constexpr uint64_t kBlockDurationNs = 10000000;
constexpr uint64_t kMixerWaitBlocks = 3; /* Allow 30 ms for sibling streams. */

std::mutex g_mutex;
std::string g_last_error = "no error";

void SetLastErrorMessage(const std::string &message) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_error = message;
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
};

std::optional<std::wstring> DefaultCaptureDeviceId() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return std::nullopt;
  }
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) {
    return std::nullopt;
  }
  LPWSTR raw_id = nullptr;
  if (FAILED(device->GetId(&raw_id)) || raw_id == nullptr) return std::nullopt;
  std::wstring id(raw_id);
  CoTaskMemFree(raw_id);
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
         (version.dwMajorVersion == 10 && version.dwBuildNumber >= 20348);
}

/*
 * WASAPI exposes one process-loopback client per target pid. Their callbacks are
 * independent even though they share a QPC clock, so forwarding them directly
 * would make the public "system" track contain overlapping, out-of-order chunks.
 *
 * Accumulate every source on that shared timeline and emit one ordered 10 ms
 * stream. A short watermark allows sibling process clients to arrive before a
 * block is committed; missing sources are silence. This mirrors the framing and
 * alignment used by production transcription pipelines.
 */
class TimelineMixer {
public:
  void Reset(size_t source_count, uint32_t channels, mrec_audio_callback callback,
             void *user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    blocks_.clear();
    progress_.assign(source_count, 0);
    source_count_ = source_count;
    channels_ = channels;
    callback_ = callback;
    user_data_ = user_data;
    have_timeline_ = false;
    next_output_block_ = 0;
    max_observed_frame_ = 0;
  }

  void Add(size_t source, const float *samples, uint32_t frame_count, bool silent,
           uint64_t host_time_ns) {
    if (frame_count == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (source >= source_count_ || channels_ == 0 || callback_ == nullptr) return;

    if (host_time_ns == 0) host_time_ns = MonotonicTimeNs();
    uint64_t block_index = host_time_ns / kBlockDurationNs;
    const uint64_t within_block_ns = host_time_ns % kBlockDurationNs;
    uint64_t frame = block_index * kFramesPerBlock +
                     (within_block_ns * kSampleRate + 500000000ull) / 1000000000ull;
    if (frame >= (block_index + 1) * kFramesPerBlock) {
      block_index++;
      frame = block_index * kFramesPerBlock;
    }

    /* QPC conversion can jitter by a frame. Preserve continuity for tiny clock
     * differences without hiding a real discontinuity. */
    const uint64_t expected = progress_[source];
    if (expected != 0) {
      const uint64_t distance = frame > expected ? frame - expected : expected - frame;
      if (distance <= 4) frame = expected;
    }

    uint32_t consumed = 0;
    while (consumed < frame_count) {
      const uint64_t current_frame = frame + consumed;
      const uint64_t current_block = current_frame / kFramesPerBlock;
      const uint32_t offset = static_cast<uint32_t>(current_frame % kFramesPerBlock);
      const uint32_t count =
          (std::min)(frame_count - consumed, kFramesPerBlock - offset);

      if (!have_timeline_) {
        have_timeline_ = true;
        next_output_block_ = current_block;
      } else if (current_block < next_output_block_) {
        /* This packet arrived after its timeline block was already committed. */
        consumed += count;
        continue;
      }

      if (!silent && samples != nullptr) {
        auto [it, inserted] = blocks_.try_emplace(current_block);
        if (inserted) {
          it->second.assign(static_cast<size_t>(kFramesPerBlock) * channels_, 0.0f);
        }
        auto &mixed = it->second;
        for (uint32_t local = 0; local < count; local++) {
          const size_t destination = static_cast<size_t>(offset + local) * channels_;
          const size_t input = static_cast<size_t>(consumed + local) * channels_;
          for (uint32_t channel = 0; channel < channels_; channel++) {
            mixed[destination + channel] += samples[input + channel];
          }
        }
      }
      consumed += count;
    }

    const uint64_t end_frame = frame + frame_count;
    progress_[source] = (std::max)(progress_[source], end_frame);
    max_observed_frame_ = (std::max)(max_observed_frame_, end_frame);
    EmitReady(false);
  }

  void Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    EmitReady(true);
    blocks_.clear();
    progress_.clear();
    source_count_ = 0;
    callback_ = nullptr;
    user_data_ = nullptr;
    have_timeline_ = false;
  }

private:
  bool EverySourceReached(uint64_t frame) const {
    if (progress_.empty()) return false;
    return std::all_of(progress_.begin(), progress_.end(),
                       [frame](uint64_t value) { return value >= frame; });
  }

  void EmitReady(bool flush_all) {
    if (!have_timeline_ || callback_ == nullptr) return;
    const uint64_t last_observed_block =
        max_observed_frame_ == 0 ? 0 : (max_observed_frame_ - 1) / kFramesPerBlock;

    while (next_output_block_ <= last_observed_block) {
      const uint64_t block_end = (next_output_block_ + 1) * kFramesPerBlock;
      const bool beyond_watermark =
          last_observed_block >= next_output_block_ + kMixerWaitBlocks;
      if (!flush_all && !EverySourceReached(block_end) && !beyond_watermark) break;

      auto it = blocks_.find(next_output_block_);
      if (it == blocks_.end()) {
        scratch_.assign(static_cast<size_t>(kFramesPerBlock) * channels_, 0.0f);
      } else {
        scratch_ = std::move(it->second);
        blocks_.erase(it);
        for (float &sample : scratch_) sample = (std::clamp)(sample, -1.0f, 1.0f);
      }
      callback_(scratch_.data(), kFramesPerBlock, channels_, kSampleRate,
                next_output_block_ * kBlockDurationNs, user_data_);
      next_output_block_++;
    }
  }

  std::mutex mutex_;
  std::map<uint64_t, std::vector<float>> blocks_;
  std::vector<uint64_t> progress_;
  std::vector<float> scratch_;
  size_t source_count_ = 0;
  uint32_t channels_ = 0;
  mrec_audio_callback callback_ = nullptr;
  void *user_data_ = nullptr;
  bool have_timeline_ = false;
  uint64_t next_output_block_ = 0;
  uint64_t max_observed_frame_ = 0;
};

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
                    void *microphone_user_data) {
    ScopedComApartment apartment;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!SupportsProcessLoopback()) {
      SetLastErrorMessage("WASAPI process loopback requires Windows build 20348 or newer");
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

    if (microphone != MREC_MICROPHONE_NONE &&
        microphone != MREC_MICROPHONE_DEFAULT) {
      SetLastErrorMessage("unknown microphone source");
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
      if (!stream) return MREC_ERR_TAP_FAILED;
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
        return MREC_ERR_TAP_FAILED;
      }
    }

    if (microphone == MREC_MICROPHONE_DEFAULT) {
      auto stream = ActivateMicrophone();
      if (!stream) {
        streams_.clear();
        return MREC_ERR_DEVICE_FAILED;
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
    recovering_.store(0, std::memory_order_relaxed);
    health_.store(MREC_CAPTURE_RUNNING, std::memory_order_release);
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

  mrec_capture_health health() const {
    return static_cast<mrec_capture_health>(health_.load(std::memory_order_acquire));
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
     *
     * Buffer duration is in 100-nanosecond units.
     */
    constexpr REFERENCE_TIME kBufferDuration = 20 * 10000; /* 20ms */
    hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration,
                                   0, &format, nullptr);
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

  std::optional<MicrophoneStream> ActivateMicrophone() {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      if (SUCCEEDED(initialized)) CoUninitialize();
      SetLastErrorMessage(HResultMessage("create audio device enumerator", hr));
      return std::nullopt;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr)) {
      if (SUCCEEDED(initialized)) CoUninitialize();
      SetLastErrorMessage(HResultMessage("get default microphone", hr));
      return std::nullopt;
    }

    MicrophoneStream stream;
    LPWSTR raw_id = nullptr;
    hr = device->GetId(&raw_id);
    if (FAILED(hr) || raw_id == nullptr) {
      if (raw_id) CoTaskMemFree(raw_id);
      if (SUCCEEDED(initialized)) CoUninitialize();
      SetLastErrorMessage(HResultMessage("read default microphone id", hr));
      return std::nullopt;
    }
    stream.device_id = raw_id;
    CoTaskMemFree(raw_id);
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(stream.client.GetAddressOf()));
    if (FAILED(hr)) {
      if (SUCCEEDED(initialized)) CoUninitialize();
      SetLastErrorMessage(HResultMessage("activate default microphone", hr));
      return std::nullopt;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    constexpr REFERENCE_TIME kBufferDuration = 20 * 10000;
    hr = stream.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        kBufferDuration, 0, &format, nullptr);
    if (FAILED(hr)) {
      if (SUCCEEDED(initialized)) CoUninitialize();
      SetLastErrorMessage(HResultMessage("initialize default microphone", hr));
      return std::nullopt;
    }

    hr = stream.client->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void **>(stream.capture.GetAddressOf()));
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(hr)) {
      SetLastErrorMessage(HResultMessage("get microphone capture client", hr));
      return std::nullopt;
    }
    return stream;
  }

  void Drain(size_t index) {
    /* Each drain thread needs its own COM apartment. */
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LoopbackStream &stream = streams_[index];

    while (!stop_requested_.load(std::memory_order_relaxed)) {
      UINT32 packet = 0;
      HRESULT hr = stream.capture->GetNextPacketSize(&packet);
      if (FAILED(hr)) {
        RestartLoopback(index, hr);
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
        RestartLoopback(index, hr);
        continue;
      }
      if (frames > 0) {
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const uint64_t timestamp =
            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || qpc == 0
                ? MonotonicTimeNs()
                : qpc * 100;
        system_mixer_.Add(index, reinterpret_cast<const float *>(data), frames,
                          silent, timestamp);
      }
      stream.capture->ReleaseBuffer(frames);
    }
    CoUninitialize();
  }

  void DrainMicrophone() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uint64_t next_device_check = 0;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
      const uint64_t now = MonotonicTimeNs();
      if (now >= next_device_check) {
        next_device_check = now + 500000000ull;
        const auto current_device = DefaultCaptureDeviceId();
        if (!microphone_ || !current_device ||
            current_device.value() != microphone_->device_id) {
          RestartMicrophone(AUDCLNT_E_DEVICE_INVALIDATED);
          continue;
        }
      }
      UINT32 packet = 0;
      if (!microphone_) break;
      HRESULT hr = microphone_->capture->GetNextPacketSize(&packet);
      if (FAILED(hr)) {
        RestartMicrophone(hr);
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
        RestartMicrophone(hr);
        continue;
      }
      if (frames > 0) {
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const uint64_t timestamp =
            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || qpc == 0
                ? MonotonicTimeNs()
                : qpc * 100;
        microphone_mixer_.Add(0, reinterpret_cast<const float *>(data), frames,
                              silent, timestamp);
      }
      microphone_->capture->ReleaseBuffer(frames);
    }
    CoUninitialize();
  }

  bool RestartLoopback(size_t index, HRESULT reason) {
    BeginRecovery();
    const uint32_t pid = streams_[index].pid;
    const bool exclude = streams_[index].exclude;
    const std::string interruption = HResultMessage("process loopback interrupted", reason);
    SetLastErrorMessage(interruption);
    if (streams_[index].client) streams_[index].client->Stop();

    for (int attempt = 0; attempt < 3; attempt++) {
      if (stop_requested_.load(std::memory_order_relaxed)) return false;
      Sleep(static_cast<DWORD>(100 * (attempt + 1)));
      auto replacement = Activate(pid, exclude);
      if (!replacement) continue;
      HRESULT hr = replacement->client->Start();
      if (FAILED(hr)) continue;
      streams_[index] = std::move(*replacement);
      SetLastErrorMessage("no error");
      EndRecovery(true);
      return true;
    }
    SetLastErrorMessage(interruption + "; recovery failed after 3 attempts");
    EndRecovery(false);
    return false;
  }

  bool RestartMicrophone(HRESULT reason) {
    BeginRecovery();
    const std::string interruption = HResultMessage("microphone capture interrupted", reason);
    SetLastErrorMessage(interruption);
    if (microphone_ && microphone_->client) microphone_->client->Stop();

    for (int attempt = 0; attempt < 3; attempt++) {
      if (stop_requested_.load(std::memory_order_relaxed)) return false;
      Sleep(static_cast<DWORD>(100 * (attempt + 1)));
      auto replacement = ActivateMicrophone();
      if (!replacement) continue;
      HRESULT hr = replacement->client->Start();
      if (FAILED(hr)) continue;
      microphone_ = std::move(*replacement);
      SetLastErrorMessage("no error");
      EndRecovery(true);
      return true;
    }
    SetLastErrorMessage(interruption + "; recovery failed after 3 attempts");
    EndRecovery(false);
    return false;
  }

  void BeginRecovery() {
    recovering_.fetch_add(1, std::memory_order_acq_rel);
    if (health_.load(std::memory_order_acquire) != MREC_CAPTURE_FAILED) {
      health_.store(MREC_CAPTURE_RECOVERING, std::memory_order_release);
    }
  }

  void EndRecovery(bool success) {
    const uint32_t remaining = recovering_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!success) {
      health_.store(MREC_CAPTURE_FAILED, std::memory_order_release);
      stop_requested_.store(true, std::memory_order_release);
    } else if (remaining == 0 &&
               health_.load(std::memory_order_acquire) != MREC_CAPTURE_FAILED) {
      health_.store(MREC_CAPTURE_RUNNING, std::memory_order_release);
    }
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
    microphone_enabled_ = false;
    callback_ = nullptr;
    user_data_ = nullptr;
    microphone_callback_ = nullptr;
    microphone_user_data_ = nullptr;
    running_ = false;
    recovering_.store(0, std::memory_order_relaxed);
    health_.store(MREC_CAPTURE_STOPPED, std::memory_order_release);
  }

  std::mutex mutex_;
  bool running_ = false;
  std::atomic<bool> stop_requested_{false};
  std::atomic<uint32_t> recovering_{0};
  std::atomic<int32_t> health_{MREC_CAPTURE_STOPPED};
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
  return MREC_PERM_NOT_REQUIRED;
}

mrec_status mrec_request_audio_permission(void) { return MREC_OK; }

mrec_permission mrec_microphone_permission_status(void) {
  return MREC_PERM_NOT_REQUIRED;
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

int32_t mrec_start_raw(const uint32_t *pids, size_t pid_count,
                         int32_t global_mixdown, int32_t mono,
                         int32_t mute_captured_output,
                         mrec_audio_callback cb, void *user_data) {
  return mrec_start_tracks_raw(
      pids, pid_count, global_mixdown, mono, mute_captured_output,
      MREC_MICROPHONE_NONE, cb, user_data, nullptr, nullptr);
}

int32_t mrec_start_tracks_raw(
    const uint32_t *pids, size_t pid_count, int32_t global_mixdown, int32_t mono,
    int32_t mute_captured_output, int32_t microphone,
    mrec_audio_callback system_audio_cb, void *system_audio_user_data,
    mrec_audio_callback microphone_cb, void *microphone_user_data) {
  /* Process loopback cannot mute the process it captures. */
  (void)mute_captured_output;
  return Session::Instance().Start(
      pids, pid_count, global_mixdown, mono, microphone, system_audio_cb,
      system_audio_user_data, microphone_cb, microphone_user_data);
}

mrec_status mrec_stop(void) { return Session::Instance().Stop(); }

int32_t mrec_is_running(void) { return Session::Instance().running() ? 1 : 0; }

mrec_capture_health mrec_capture_health_status(void) {
  return Session::Instance().health();
}

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
