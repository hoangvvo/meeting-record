/*
 * N-API binding for meeting-record.
 *
 * mrec_start() delivers PCM on a realtime audio thread, where calling into V8,
 * allocating, or locking is not allowed:
 *
 *   audio thread : memcpy into a preallocated ring buffer, then wake JS
 *   JS thread    : drain the ring into a Buffer and push it downstream
 *
 * When the consumer falls behind, the ring drops incoming samples and reports
 * the count to JS.
 */
#include <napi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

extern "C" {
#include "meeting-record-detect.h"
#include "meeting-record.h"
}

namespace {

class RingBuffer {
public:
  struct Chunk {
    std::vector<float> samples;
    uint32_t frame_count = 0;
    uint32_t channels = 0;
    double sample_rate = 0;
    uint64_t host_time_ns = 0;
    size_t dropped = 0;
  };

  explicit RingBuffer(size_t capacity)
      : buffer_(capacity), mask_(capacity - 1), packets_(1 << 12),
        packet_mask_(packets_.size() - 1) {}

  /* Realtime-safe: no allocation, no locks. */
  bool Write(const float *data, uint32_t frame_count, uint32_t channels,
             double sample_rate, uint64_t host_time_ns,
             const std::atomic<bool> *paused) {
    const size_t count = static_cast<size_t>(frame_count) * channels;
    if (writing_.test_and_set(std::memory_order_acquire)) {
      dropped_pending_.fetch_add(count, std::memory_order_relaxed);
      return false;
    }
    if (paused && paused->load(std::memory_order_acquire)) {
      writing_.clear(std::memory_order_release);
      return false;
    }

    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t free_space = buffer_.size() - (head - tail);
    const size_t packet_head = packet_head_.load(std::memory_order_relaxed);
    const size_t packet_tail = packet_tail_.load(std::memory_order_acquire);

    if (count == 0 || count > free_space ||
        packet_head - packet_tail >= packets_.size()) {
      dropped_pending_.fetch_add(count, std::memory_order_relaxed);
      writing_.clear(std::memory_order_release);
      return false;
    }

    for (size_t i = 0; i < count; i++) {
      buffer_[(head + i) & mask_] = data[i];
    }
    packets_[packet_head & packet_mask_] = {
        head, count, frame_count, channels, sample_rate, host_time_ns,
        dropped_pending_.exchange(0, std::memory_order_relaxed)};
    head_.store(head + count, std::memory_order_release);
    packet_head_.store(packet_head + 1, std::memory_order_release);
    writing_.clear(std::memory_order_release);
    return true;
  }

  bool Read(Chunk &out) {
    const size_t packet_tail = packet_tail_.load(std::memory_order_relaxed);
    const size_t packet_head = packet_head_.load(std::memory_order_acquire);
    if (packet_tail == packet_head) return false;

    const Packet first = packets_[packet_tail & packet_mask_];
    size_t next_packet = packet_tail;
    size_t sample_count = 0;
    uint32_t frame_count = 0;
    while (next_packet < packet_head) {
      const Packet packet = packets_[next_packet & packet_mask_];
      if (packet.channels != first.channels || packet.sample_rate != first.sample_rate) break;
      if (next_packet != packet_tail &&
          (packet.dropped_before != 0 ||
           !IsContiguous(first, frame_count, packet))) {
        break;
      }
      sample_count += packet.sample_count;
      frame_count += packet.frame_count;
      next_packet++;
    }

    out.samples.resize(sample_count);
    for (size_t i = 0; i < sample_count; i++) {
      out.samples[i] = buffer_[(first.sample_start + i) & mask_];
    }
    out.frame_count = frame_count;
    out.channels = first.channels;
    out.sample_rate = first.sample_rate;
    out.host_time_ns = first.host_time_ns;
    out.dropped = first.dropped_before;

    tail_.store(first.sample_start + sample_count, std::memory_order_release);
    packet_tail_.store(next_packet, std::memory_order_release);
    return true;
  }

  bool HasData() const {
    return packet_tail_.load(std::memory_order_acquire) !=
           packet_head_.load(std::memory_order_acquire);
  }

  void Clear() {
    while (writing_.test_and_set(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    packet_tail_.store(packet_head_.load(std::memory_order_acquire),
                       std::memory_order_release);
    dropped_pending_.store(0, std::memory_order_relaxed);
    writing_.clear(std::memory_order_release);
  }

private:
  struct Packet {
    size_t sample_start = 0;
    size_t sample_count = 0;
    uint32_t frame_count = 0;
    uint32_t channels = 0;
    double sample_rate = 0;
    uint64_t host_time_ns = 0;
    size_t dropped_before = 0;
  };

  static bool IsContiguous(const Packet &first, uint32_t preceding_frames,
                           const Packet &next) {
    if (first.host_time_ns == 0 || next.host_time_ns == 0 ||
        first.sample_rate <= 0) {
      return true;
    }
    const auto elapsed = static_cast<uint64_t>(
        static_cast<long double>(preceding_frames) * 1000000000.0L /
            first.sample_rate +
        0.5L);
    const uint64_t expected = first.host_time_ns + elapsed;
    const uint64_t distance = expected > next.host_time_ns
                                  ? expected - next.host_time_ns
                                  : next.host_time_ns - expected;
    return distance <= 2000000; /* Tolerate 2 ms of callback-clock jitter. */
  }

  std::vector<float> buffer_;
  size_t mask_;
  std::vector<Packet> packets_;
  size_t packet_mask_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  std::atomic<size_t> packet_head_{0};
  std::atomic<size_t> packet_tail_{0};
  std::atomic<size_t> dropped_pending_{0};
  std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
};

struct CaptureContext;

struct TrackContext {
  TrackContext(CaptureContext *capture, bool microphone)
      : capture(capture), microphone(microphone) {}

  RingBuffer ring{1 << 20}; /* ~10s of mono 48k */
  Napi::ThreadSafeFunction tsfn;
  std::atomic<double> sample_rate{0};
  std::atomic<uint32_t> channels{0};
  std::atomic<bool> active{false};
  std::atomic<bool> delivery_enabled{false};
  std::atomic<bool> wake_pending{false};
  CaptureContext *capture = nullptr;
  bool microphone = false;
};

struct CaptureContext {
  explicit CaptureContext(bool include_microphone)
      : id(next_id.fetch_add(1, std::memory_order_relaxed)),
        system_audio(this, false) {
    if (include_microphone) {
      microphone = std::make_unique<TrackContext>(this, true);
    }
  }

  static std::atomic<uint64_t> next_id;
  uint64_t id;
  TrackContext system_audio;
  std::unique_ptr<TrackContext> microphone;
  std::atomic<bool> paused{false};
};

std::atomic<uint64_t> CaptureContext::next_id{1};

std::unique_ptr<CaptureContext> g_capture;

TrackContext *CurrentTrack(bool microphone) {
  if (!g_capture) return nullptr;
  return microphone ? g_capture->microphone.get() : &g_capture->system_audio;
}

void ScheduleTrackWake(TrackContext *track);

void ReleaseCapture() {
  if (!g_capture) return;
  g_capture->system_audio.active.store(false, std::memory_order_release);
  g_capture->system_audio.delivery_enabled.store(false, std::memory_order_release);
  g_capture->system_audio.tsfn.Release();
  if (g_capture->microphone) {
    g_capture->microphone->active.store(false, std::memory_order_release);
    g_capture->microphone->delivery_enabled.store(false, std::memory_order_release);
    g_capture->microphone->tsfn.Release();
  }
  g_capture.reset();
}

void OnAudio(const float *frames, uint32_t frame_count, uint32_t channels,
             double sample_rate, uint64_t host_time_ns, void *user_data) {
  auto *track = static_cast<TrackContext *>(user_data);
  if (!track || !track->active.load(std::memory_order_relaxed) ||
      track->capture->paused.load(std::memory_order_relaxed)) {
    return;
  }

  track->sample_rate.store(sample_rate, std::memory_order_relaxed);
  track->channels.store(channels, std::memory_order_relaxed);
  track->ring.Write(frames, frame_count, channels, sample_rate, host_time_ns,
                    &track->capture->paused);

  if (track->delivery_enabled.load(std::memory_order_acquire) &&
      track->ring.HasData()) {
    ScheduleTrackWake(track);
  }
}

void ScheduleTrackWake(TrackContext *track) {
  if (!track || !track->active.load(std::memory_order_acquire) ||
      !track->delivery_enabled.load(std::memory_order_acquire)) {
    return;
  }
  bool expected = false;
  if (!track->wake_pending.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
    return;
  }
  const bool microphone = track->microphone;
  const uint64_t capture_id = track->capture->id;
  const napi_status status = track->tsfn.NonBlockingCall(
      [microphone, capture_id](Napi::Env env, Napi::Function callback) {
    if (!g_capture || g_capture->id != capture_id) return;
    TrackContext *track = CurrentTrack(microphone);
    if (!track) return;
    track->wake_pending.store(false, std::memory_order_release);
    if (!track->delivery_enabled.load(std::memory_order_acquire)) return;

    RingBuffer::Chunk chunk;
    if (track->ring.Read(chunk)) {
      Napi::Object info = Napi::Object::New(env);
      info.Set("sampleRate", chunk.sample_rate);
      info.Set("channels", chunk.channels);
      info.Set("frameCount", chunk.frame_count);
      info.Set("hostTimeNs", Napi::BigInt::New(env, chunk.host_time_ns));
      info.Set("dropped", static_cast<double>(chunk.dropped));

      callback.Call({Napi::Buffer<float>::Copy(env, chunk.samples.data(),
                                               chunk.samples.size()),
                     info});
    }
    if (track->ring.HasData()) ScheduleTrackWake(track);
  });
  if (status != napi_ok) {
    track->wake_pending.store(false, std::memory_order_release);
  }
}

/* mrec_start can block for seconds while the permission grant is undetermined. */
class StartWorker : public Napi::AsyncWorker {
public:
  StartWorker(Napi::Env env, uint32_t pid, bool have_pid, bool system_wide,
              bool mono, bool microphone)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        pid_(pid), have_pid_(have_pid), system_wide_(system_wide), mono_(mono),
        microphone_(microphone) {}

  Napi::Promise Promise() { return deferred_.Promise(); }

  void Execute() override {
    std::vector<uint32_t> resolved_pids;

    if (have_pid_) {
#ifdef _WIN32
      /* Process-loopback INCLUDE semantics already follows the visible app's
       * child process tree, including helpers created after capture starts.
       * Capturing the currently active helpers as separate clients would double
       * count their audio and break when those helpers respawn. */
      resolved_pids.push_back(pid_);
#else
      /* A meeting's visible app pid is often not where Chrome, Teams, or
       * Electron render audio. Resolve its current helper pids here so the JS
       * API only needs one stable OS identifier. An arbitrary audio-process pid
       * that is not a detected meeting remains a valid direct target. */
      mrec_meeting meetings[16]{};
      size_t count = 0;
      mrec_scan(meetings, 16, &count);
      const size_t meeting_count = std::min(count, static_cast<size_t>(16));
      for (size_t i = 0; i < meeting_count; i++) {
        if (meetings[i].pid != pid_) continue;
        const size_t pid_count =
            std::min(meetings[i].audio_pid_count, static_cast<size_t>(MREC_MAX_AUDIO_PIDS));
        resolved_pids.assign(meetings[i].audio_pids,
                             meetings[i].audio_pids + pid_count);
        resolved_pids.erase(std::remove(resolved_pids.begin(), resolved_pids.end(), 0),
                            resolved_pids.end());
        break;
      }
      if (resolved_pids.empty()) resolved_pids.push_back(pid_);
#endif
    }

    mrec_config cfg;
    mrec_config_defaults(&cfg);
    cfg.mono = mono_ ? 1 : 0;
    if (system_wide_) {
      cfg.global_mixdown = 1;
    } else {
      cfg.pids = resolved_pids.data();
      cfg.pid_count = resolved_pids.size();
    }
    status_ = mrec_start_tracks_raw(
        cfg.pids, cfg.pid_count, cfg.global_mixdown, cfg.mono,
        cfg.mute_captured_output,
        microphone_ ? MREC_MICROPHONE_DEFAULT : MREC_MICROPHONE_NONE, OnAudio,
        &g_capture->system_audio, microphone_ ? OnAudio : nullptr,
        microphone_ ? g_capture->microphone.get() : nullptr);
    if (status_ != MREC_OK) error_ = mrec_last_error();
  }

  void OnOK() override {
    Napi::Env env = Env();
    if (status_ != MREC_OK) {
      Napi::Error err = Napi::Error::New(env, error_);
      err.Set("code", Napi::Number::New(env, status_));
      ReleaseCapture();
      deferred_.Reject(err.Value());
      return;
    }
    double rate = 0;
    uint32_t channels = 0;
    const mrec_status format_status = mrec_current_format(&rate, &channels);
    if (format_status != MREC_OK ||
        mrec_capture_health_status() == MREC_CAPTURE_FAILED) {
      const int32_t code = format_status == MREC_OK ? MREC_ERR_DEVICE_FAILED
                                                    : format_status;
      Napi::Error err = Napi::Error::New(env, mrec_last_error());
      err.Set("code", Napi::Number::New(env, code));
      mrec_stop();
      ReleaseCapture();
      deferred_.Reject(err.Value());
      return;
    }

    Napi::Object system_audio = Napi::Object::New(env);
    system_audio.Set("sampleRate", rate);
    system_audio.Set("channels", channels);

    Napi::Object result = Napi::Object::New(env);
    result.Set("systemAudio", system_audio);
    if (microphone_) {
      double microphone_rate = 0;
      uint32_t microphone_channels = 0;
      const mrec_status microphone_format_status =
          mrec_current_microphone_format(&microphone_rate, &microphone_channels);
      if (microphone_format_status != MREC_OK) {
        Napi::Error err = Napi::Error::New(env, mrec_last_error());
        err.Set("code", Napi::Number::New(env, microphone_format_status));
        mrec_stop();
        ReleaseCapture();
        deferred_.Reject(err.Value());
        return;
      }
      Napi::Object microphone = Napi::Object::New(env);
      microphone.Set("sampleRate", microphone_rate);
      microphone.Set("channels", microphone_channels);
      result.Set("microphone", microphone);
    }
    deferred_.Resolve(result);

    /* Audio can begin before this worker returns to JS. Keep it in the bounded
     * native ring until the start promise has been resolved, then schedule one
     * coalesced drain per track. */
    if (g_capture) {
      g_capture->system_audio.delivery_enabled.store(true, std::memory_order_release);
      ScheduleTrackWake(&g_capture->system_audio);
      if (g_capture->microphone) {
        g_capture->microphone->delivery_enabled.store(true, std::memory_order_release);
        ScheduleTrackWake(g_capture->microphone.get());
      }
    }
  }

private:
  Napi::Promise::Deferred deferred_;
  uint32_t pid_;
  bool have_pid_;
  bool system_wide_;
  bool mono_;
  bool microphone_;
  int32_t status_ = MREC_OK;
  std::string error_;
};

Napi::Object MeetingToJS(Napi::Env env, const mrec_meeting *m) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("platform", Napi::String::New(env, mrec_platform_id(m->platform)));
  out.Set("pid", Napi::Number::New(env, m->pid));
  out.Set("appName", Napi::String::New(env, m->app_name));
  out.Set("title", Napi::String::New(env, m->title));
  out.Set("url", Napi::String::New(env, m->url));
  out.Set("isUsingMic", Napi::Boolean::New(env, m->is_using_mic != 0));
  out.Set("isPlayingAudio", Napi::Boolean::New(env, m->is_playing_audio != 0));
  out.Set("confidence", Napi::Number::New(env, m->confidence));
  out.Set("shouldRecord", Napi::Boolean::New(env, m->should_record != 0));
  return out;
}

Napi::ThreadSafeFunction g_meeting_tsfn;
std::atomic<bool> g_watching{false};
std::atomic<uint64_t> g_watch_generation{0};

/* Runs on the library's serial queue, so allocation is allowed. */
void OnMeeting(const mrec_meeting *meeting, mrec_event event, void *user_data) {
  if (!g_watching.load(std::memory_order_relaxed)) return;
  const uint64_t generation =
      g_watch_generation.load(std::memory_order_acquire);
  auto *copy = new mrec_meeting(*meeting);
  const int32_t kind = event;

  const napi_status status = g_meeting_tsfn.NonBlockingCall(
      copy, [kind, generation](Napi::Env env, Napi::Function callback,
                              mrec_meeting *m) {
        std::unique_ptr<mrec_meeting> owned(m);
        if (!g_watching.load(std::memory_order_acquire) ||
            g_watch_generation.load(std::memory_order_acquire) != generation) {
          return;
        }
        static const char *kNames[] = {"started", "updated", "ended"};
        const char *name = (kind >= 0 && kind <= 2) ? kNames[kind] : "updated";
        callback.Call({Napi::String::New(env, name), MeetingToJS(env, owned.get())});
      });
  if (status != napi_ok) delete copy;
}

Napi::Value AudioPermissionStatus(const Napi::CallbackInfo &info) {
  static const char *kNames[] = {"unknown", "granted", "denied", "not-required"};
  const int status = mrec_audio_permission_status();
  return Napi::String::New(info.Env(),
                           (status >= 0 && status <= 3) ? kNames[status] : "unknown");
}

Napi::Value RequestAudioPermission(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), mrec_request_audio_permission());
}

Napi::Value MicrophonePermissionStatus(const Napi::CallbackInfo &info) {
  static const char *kNames[] = {"unknown", "granted", "denied", "not-required"};
  const int status = mrec_microphone_permission_status();
  return Napi::String::New(info.Env(),
                           (status >= 0 && status <= 3) ? kNames[status] : "unknown");
}

Napi::Value RequestMicrophonePermission(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), mrec_request_microphone_permission());
}

Napi::Value AccessibilityPermissionStatus(const Napi::CallbackInfo &info) {
  static const char *kNames[] = {"unknown", "granted", "denied", "not-required"};
  const int status = mrec_accessibility_permission_status();
  return Napi::String::New(info.Env(),
                           (status >= 0 && status <= 3) ? kNames[status] : "unknown");
}

Napi::Value RequestAccessibilityPermission(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), mrec_request_accessibility_permission());
}

Napi::Value Scan(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  mrec_meeting meetings[16]{};
  size_t count = 0;
  if (mrec_scan(meetings, 16, &count) < 0 && count == 0) {
    return Napi::Array::New(env, 0);
  }
  const size_t meeting_count = std::min(count, static_cast<size_t>(16));
  Napi::Array out = Napi::Array::New(env, meeting_count);
  for (size_t i = 0; i < meeting_count; i++) out.Set(i, MeetingToJS(env, &meetings[i]));
  return out;
}

Napi::Value WatchStart(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (g_watching.load()) return Napi::Number::New(env, MREC_ERR_ALREADY_RUNNING);
  if (info.Length() == 0 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "meeting callback is required")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  g_watch_generation.fetch_add(1, std::memory_order_acq_rel);
  g_meeting_tsfn = Napi::ThreadSafeFunction::New(
      env, info[0].As<Napi::Function>(), "mrec-meetings", 0, 1);
  g_watching.store(true);

  const int32_t status = mrec_watch_start(OnMeeting, nullptr);
  if (status != MREC_OK) {
    g_watching.store(false);
    g_watch_generation.fetch_add(1, std::memory_order_acq_rel);
    g_meeting_tsfn.Release();
  }
  return Napi::Number::New(env, status);
}

Napi::Value WatchStop(const Napi::CallbackInfo &info) {
  const bool was_watching = g_watching.exchange(false);
  g_watch_generation.fetch_add(1, std::memory_order_acq_rel);
  const int32_t status = mrec_watch_stop();
  if (was_watching) g_meeting_tsfn.Release();
  return Napi::Number::New(info.Env(), status);
}

Napi::Value IsWatching(const Napi::CallbackInfo &info) {
  return Napi::Boolean::New(info.Env(), mrec_is_watching() != 0);
}

Napi::Value CaptureStart(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object opts = info[0].As<Napi::Object>();
  Napi::Function on_system_audio = info[1].As<Napi::Function>();

  const bool have_pid = opts.Has("pid") && opts.Get("pid").IsNumber();
  const uint32_t pid = have_pid ? opts.Get("pid").As<Napi::Number>().Uint32Value() : 0;
  const bool system_wide =
      opts.Has("systemWide") && opts.Get("systemWide").ToBoolean().Value();
  if ((have_pid && pid == 0) || have_pid == system_wide) {
    Napi::TypeError::New(env, "provide exactly one capture target: pid or systemWide")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (g_capture || mrec_is_running()) {
    Napi::Error err = Napi::Error::New(env, "capture already running");
    err.Set("code", Napi::Number::New(env, MREC_ERR_ALREADY_RUNNING));
    err.ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const bool mono = !opts.Has("mono") || opts.Get("mono").ToBoolean().Value();
  const bool microphone =
      opts.Has("microphone") && opts.Get("microphone").ToBoolean().Value();
  if (microphone && (info.Length() < 3 || !info[2].IsFunction())) {
    Napi::TypeError::New(env, "microphone callback is required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  g_capture = std::make_unique<CaptureContext>(microphone);
  g_capture->system_audio.tsfn = Napi::ThreadSafeFunction::New(
      env, on_system_audio, "mrec-system-audio", 1, 1);
  g_capture->system_audio.active.store(true);
  if (microphone) {
    g_capture->microphone->tsfn = Napi::ThreadSafeFunction::New(
        env, info[2].As<Napi::Function>(), "mrec-microphone", 1, 1);
    g_capture->microphone->active.store(true);
  }

  auto *worker =
      new StartWorker(env, pid, have_pid, system_wide, mono, microphone);
  Napi::Promise promise = worker->Promise();
  worker->Queue();
  return promise;
}

Napi::Value CaptureStop(const Napi::CallbackInfo &info) {
  const int32_t status = mrec_stop();
  ReleaseCapture();
  return Napi::Number::New(info.Env(), status);
}

Napi::Value CapturePause(const Napi::CallbackInfo &info) {
  if (!g_capture || !mrec_is_running()) {
    Napi::Error::New(info.Env(), "capture is not running").ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  g_capture->paused.store(true, std::memory_order_release);
  g_capture->system_audio.ring.Clear();
  if (g_capture->microphone) g_capture->microphone->ring.Clear();
  return info.Env().Undefined();
}

Napi::Value CaptureResume(const Napi::CallbackInfo &info) {
  if (!g_capture || !mrec_is_running()) {
    Napi::Error::New(info.Env(), "capture is not running").ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  g_capture->system_audio.ring.Clear();
  if (g_capture->microphone) g_capture->microphone->ring.Clear();
  g_capture->paused.store(false, std::memory_order_release);
  return info.Env().Undefined();
}

Napi::Value CaptureSetConsumerReady(const Napi::CallbackInfo &info) {
  if (!g_capture || info.Length() < 2) return info.Env().Undefined();
  const bool microphone = info[0].ToBoolean().Value();
  const bool ready = info[1].ToBoolean().Value();
  TrackContext *track = CurrentTrack(microphone);
  if (!track) return info.Env().Undefined();
  track->delivery_enabled.store(ready, std::memory_order_release);
  if (ready && track->ring.HasData()) ScheduleTrackWake(track);
  return info.Env().Undefined();
}

Napi::Value IsRunning(const Napi::CallbackInfo &info) {
  return Napi::Boolean::New(info.Env(), mrec_is_running() != 0);
}

Napi::Value CaptureHealthStatus(const Napi::CallbackInfo &info) {
  static const char *kNames[] = {"stopped", "running", "recovering", "failed"};
  const int status = mrec_capture_health_status();
  return Napi::String::New(info.Env(),
                           (status >= 0 && status <= 3) ? kNames[status] : "failed");
}

Napi::Value LastError(const Napi::CallbackInfo &info) {
  return Napi::String::New(info.Env(), mrec_last_error());
}

Napi::Value ListAudioProcesses(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  mrec_process procs[256]{};
  size_t count = 0;
  mrec_list_audio_processes(procs, 256, &count);

  const size_t process_count = std::min(count, static_cast<size_t>(256));
  Napi::Array out = Napi::Array::New(env, process_count);
  for (size_t i = 0; i < process_count; i++) {
    Napi::Object p = Napi::Object::New(env);
    p.Set("pid", Napi::Number::New(env, procs[i].pid));
    p.Set("name", Napi::String::New(env, procs[i].name));
    p.Set("bundleId", Napi::String::New(env, procs[i].bundle_id));
    p.Set("isPlayingAudio", Napi::Boolean::New(env, procs[i].is_running_output != 0));
    p.Set("isUsingMic", Napi::Boolean::New(env, procs[i].is_running_input != 0));
    out.Set(i, p);
  }
  return out;
}

} // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("audioPermissionStatus", Napi::Function::New(env, AudioPermissionStatus));
  exports.Set("requestAudioPermission", Napi::Function::New(env, RequestAudioPermission));
  exports.Set("microphonePermissionStatus",
              Napi::Function::New(env, MicrophonePermissionStatus));
  exports.Set("requestMicrophonePermission",
              Napi::Function::New(env, RequestMicrophonePermission));
  exports.Set("accessibilityPermissionStatus",
              Napi::Function::New(env, AccessibilityPermissionStatus));
  exports.Set("requestAccessibilityPermission",
              Napi::Function::New(env, RequestAccessibilityPermission));

  exports.Set("scan", Napi::Function::New(env, Scan));
  exports.Set("watchStart", Napi::Function::New(env, WatchStart));
  exports.Set("watchStop", Napi::Function::New(env, WatchStop));
  exports.Set("isWatching", Napi::Function::New(env, IsWatching));

  exports.Set("captureStart", Napi::Function::New(env, CaptureStart));
  exports.Set("captureStop", Napi::Function::New(env, CaptureStop));
  exports.Set("capturePause", Napi::Function::New(env, CapturePause));
  exports.Set("captureResume", Napi::Function::New(env, CaptureResume));
  exports.Set("captureSetConsumerReady",
              Napi::Function::New(env, CaptureSetConsumerReady));
  exports.Set("isRunning", Napi::Function::New(env, IsRunning));
  exports.Set("captureHealthStatus", Napi::Function::New(env, CaptureHealthStatus));
  exports.Set("lastError", Napi::Function::New(env, LastError));
  exports.Set("listAudioProcesses", Napi::Function::New(env, ListAudioProcesses));
  return exports;
}

NODE_API_MODULE(meeting_record, Init)
