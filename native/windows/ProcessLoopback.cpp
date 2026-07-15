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
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/implements.h>

#include <atomic>
#include <cstdio>
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
  uint32_t channels = 0;
};

class Session {
public:
  static Session &Instance() {
    static Session session;
    return session;
  }

  mrec_status Start(const uint32_t *pids, size_t pid_count, int32_t global_mixdown,
                      int32_t mono, mrec_audio_callback cb, void *user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
      SetLastErrorMessage("capture already running");
      return MREC_ERR_ALREADY_RUNNING;
    }

    std::vector<uint32_t> targets(pids, pids + pid_count);
    if (targets.empty() && !global_mixdown) {
      SetLastErrorMessage("no pids supplied and global_mixdown not set");
      return MREC_ERR_NO_PROCESSES;
    }

    callback_ = cb;
    user_data_ = user_data;
    channels_ = mono ? 1 : 2;

    /* One client per target process; pid 0 with EXCLUDE semantics is system-wide. */
    if (global_mixdown) {
      auto stream = Activate(0, /*exclude=*/true);
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

    running_ = true;
    stop_requested_ = false;
    for (size_t i = 0; i < streams_.size(); i++) {
      HRESULT hr = streams_[i].client->Start();
      if (FAILED(hr)) {
        SetLastErrorMessage(HResultMessage("IAudioClient::Start", hr));
        StopLocked();
        return MREC_ERR_IOPROC_FAILED;
      }
      workers_.emplace_back([this, i] { Drain(i); });
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

  void Drain(size_t index) {
    /* Each drain thread needs its own COM apartment. */
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LoopbackStream &stream = streams_[index];

    while (!stop_requested_.load(std::memory_order_relaxed)) {
      UINT32 packet = 0;
      if (FAILED(stream.capture->GetNextPacketSize(&packet)) || packet == 0) {
        Sleep(5);
        continue;
      }
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0, qpc = 0;
      if (FAILED(stream.capture->GetBuffer(&data, &frames, &flags, &position, &qpc))) {
        continue;
      }
      if (frames > 0 && callback_ != nullptr) {
        /* SILENT means the buffer contents are undefined; emit zeros instead. */
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          silence_.assign(static_cast<size_t>(frames) * stream.channels, 0.0f);
          callback_(silence_.data(), frames, stream.channels, kSampleRate,
                    qpc * 100, user_data_);
        } else {
          callback_(reinterpret_cast<const float *>(data), frames, stream.channels,
                    kSampleRate, qpc * 100, user_data_);
        }
      }
      stream.capture->ReleaseBuffer(frames);
    }
    CoUninitialize();
  }

  void StopLocked() {
    stop_requested_ = true;
    for (auto &worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
    for (auto &stream : streams_) {
      if (stream.client) stream.client->Stop();
    }
    streams_.clear();
    callback_ = nullptr;
    user_data_ = nullptr;
    running_ = false;
  }

  std::mutex mutex_;
  bool running_ = false;
  std::atomic<bool> stop_requested_{false};
  std::vector<LoopbackStream> streams_;
  std::vector<std::thread> workers_;
  std::vector<float> silence_;
  mrec_audio_callback callback_ = nullptr;
  void *user_data_ = nullptr;
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

mrec_status mrec_list_audio_processes(mrec_process *out, size_t capacity,
                                          size_t *out_count) {
  if (!out || !out_count) return MREC_ERR_INTERNAL;

  /*
   * No per-process audio-activity query exists here, so enumerate processes and
   * leave is_running_output at 0 to mean unknown.
   */
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return MREC_ERR_INTERNAL;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  size_t count = 0;
  size_t total = 0;

  if (Process32FirstW(snapshot, &entry)) {
    do {
      total++;
      if (count >= capacity) continue;
      mrec_process &slot = out[count++];
      slot.pid = entry.th32ProcessID;
      slot.is_running_output = 0;
      slot.is_running_input = 0;
      slot.bundle_id[0] = '\0';
      WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, slot.name,
                          sizeof slot.name, nullptr, nullptr);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  *out_count = count;
  return total > capacity ? MREC_ERR_BUFFER_TOO_SMALL : MREC_OK;
}

int32_t mrec_start_raw(const uint32_t *pids, size_t pid_count,
                         int32_t global_mixdown, int32_t mono,
                         int32_t mute_captured_output,
                         mrec_audio_callback cb, void *user_data) {
  /* Process loopback cannot mute the process it captures. */
  (void)mute_captured_output;
  return Session::Instance().Start(pids, pid_count, global_mixdown, mono, cb,
                                   user_data);
}

mrec_status mrec_stop(void) { return Session::Instance().Stop(); }

int32_t mrec_is_running(void) { return Session::Instance().running() ? 1 : 0; }

mrec_status mrec_current_format(double *sample_rate, uint32_t *channels) {
  if (!Session::Instance().running()) return MREC_ERR_NOT_RUNNING;
  Session::Instance().Format(sample_rate, channels);
  return MREC_OK;
}

const char *mrec_last_error(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_last_error.c_str();
}

} // extern "C"
