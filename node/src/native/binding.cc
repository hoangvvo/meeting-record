/*
 * N-API binding for meeting-record.
 *
 * The only genuinely hard part is the audio path. mrec_start() delivers PCM on a
 * realtime audio thread, where calling into V8 — or allocating, or taking a lock —
 * is not allowed. So:
 *
 *   audio thread : memcpy into a lock-free SPSC ring buffer, then wake JS
 *   JS thread    : drain the ring into a Buffer and push it downstream
 *
 * If the consumer cannot keep up, the ring overwrites its oldest samples and
 * counts the loss. Silently dropping audio in a recorder is worse than saying so,
 * hence the drop count surfacing to JS.
 */
#include <napi.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include "meeting-record-detect.h"
#include "meeting-record.h"
}

namespace {

/* ---- ring buffer ------------------------------------------------------- */

/*
 * Single producer (audio thread), single consumer (JS thread). Power-of-two
 * capacity so the modulo is a mask.
 */
class RingBuffer {
public:
  explicit RingBuffer(size_t capacity) : buffer_(capacity), mask_(capacity - 1) {}

  /* Realtime-safe: no allocation, no locks. */
  void Write(const float *data, size_t count) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t free_space = buffer_.size() - (head - tail) - 1;

    if (count > free_space) {
      /* Overrun: advance the reader past what we are about to clobber. */
      const size_t overflow = count - free_space;
      tail_.store(tail + overflow, std::memory_order_release);
      dropped_.fetch_add(overflow, std::memory_order_relaxed);
    }

    for (size_t i = 0; i < count; i++) {
      buffer_[(head + i) & mask_] = data[i];
    }
    head_.store(head + count, std::memory_order_release);
  }

  size_t Read(std::vector<float> &out) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t available = head - tail;
    if (available == 0) return 0;

    out.resize(available);
    for (size_t i = 0; i < available; i++) {
      out[i] = buffer_[(tail + i) & mask_];
    }
    tail_.store(tail + available, std::memory_order_release);
    return available;
  }

  size_t TakeDropped() { return dropped_.exchange(0, std::memory_order_relaxed); }

private:
  std::vector<float> buffer_;
  size_t mask_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  std::atomic<size_t> dropped_{0};
};

/* ---- capture ----------------------------------------------------------- */

struct CaptureContext {
  RingBuffer ring{1 << 20}; /* ~1M floats: 10s of mono 48k, ample slack */
  Napi::ThreadSafeFunction tsfn;
  std::atomic<double> sample_rate{0};
  std::atomic<uint32_t> channels{0};
  std::atomic<bool> active{false};
};

std::unique_ptr<CaptureContext> g_capture;

/*
 * Realtime audio thread. Copy, record the format, wake the JS side. Nothing here
 * allocates: the ring is preallocated and NonBlockingCall does not block.
 */
void OnAudio(const float *frames, uint32_t frame_count, uint32_t channels,
             double sample_rate, uint64_t host_time_ns, void *user_data) {
  auto *ctx = static_cast<CaptureContext *>(user_data);
  if (!ctx || !ctx->active.load(std::memory_order_relaxed)) return;

  ctx->sample_rate.store(sample_rate, std::memory_order_relaxed);
  ctx->channels.store(channels, std::memory_order_relaxed);
  ctx->ring.Write(frames, static_cast<size_t>(frame_count) * channels);

  ctx->tsfn.NonBlockingCall([](Napi::Env env, Napi::Function callback) {
    if (!g_capture) return;
    std::vector<float> chunk;
    if (g_capture->ring.Read(chunk) == 0) return;

    const size_t dropped = g_capture->ring.TakeDropped();
    Napi::Object info = Napi::Object::New(env);
    info.Set("sampleRate", g_capture->sample_rate.load());
    info.Set("channels", g_capture->channels.load());
    info.Set("dropped", static_cast<double>(dropped));

    callback.Call({Napi::Buffer<float>::Copy(env, chunk.data(), chunk.size()), info});
  });
}

/*
 * mrec_start can block for several seconds the first time, while the permission
 * grant is still undetermined, so it must not run on the JS thread.
 */
class StartWorker : public Napi::AsyncWorker {
public:
  StartWorker(Napi::Env env, std::vector<uint32_t> pids, bool system_wide, bool mono,
              const mrec_meeting *meeting, bool have_meeting)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        pids_(std::move(pids)), system_wide_(system_wide), mono_(mono),
        have_meeting_(have_meeting) {
    if (have_meeting) meeting_ = *meeting;
  }

  Napi::Promise Promise() { return deferred_.Promise(); }

  void Execute() override {
    if (have_meeting_) {
      status_ = mrec_start_meeting(&meeting_, OnAudio, g_capture.get());
    } else {
      mrec_config cfg;
      mrec_config_defaults(&cfg);
      cfg.mono = mono_ ? 1 : 0;
      if (system_wide_) {
        cfg.global_mixdown = 1;
      } else if (!pids_.empty()) {
        cfg.pids = pids_.data();
        cfg.pid_count = pids_.size();
      }
      status_ = mrec_start(&cfg, OnAudio, g_capture.get());
    }
    if (status_ != MREC_OK) error_ = mrec_last_error();
  }

  void OnOK() override {
    Napi::Env env = Env();
    if (status_ != MREC_OK) {
      Napi::Error err = Napi::Error::New(env, error_);
      err.Set("code", Napi::Number::New(env, status_));
      deferred_.Reject(err.Value());
      return;
    }
    double rate = 0;
    uint32_t channels = 0;
    mrec_current_format(&rate, &channels);

    Napi::Object result = Napi::Object::New(env);
    result.Set("sampleRate", rate);
    result.Set("channels", channels);
    deferred_.Resolve(result);
  }

private:
  Napi::Promise::Deferred deferred_;
  std::vector<uint32_t> pids_;
  bool system_wide_;
  bool mono_;
  bool have_meeting_;
  mrec_meeting meeting_{};
  int32_t status_ = MREC_OK;
  std::string error_;
};

/* ---- meeting marshalling ---------------------------------------------- */

Napi::Object MeetingToJS(Napi::Env env, const mrec_meeting *m) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("platform", Napi::String::New(env, mrec_platform_id(m->platform)));
  /* Non-enumerable in the TS layer: only used to round-trip back into C. */
  out.Set("platformCode", Napi::Number::New(env, m->platform));
  out.Set("pid", Napi::Number::New(env, m->pid));
  out.Set("appName", Napi::String::New(env, m->app_name));
  out.Set("title", Napi::String::New(env, m->title));
  out.Set("url", Napi::String::New(env, m->url));
  out.Set("isUsingMic", Napi::Boolean::New(env, m->is_using_mic != 0));
  out.Set("isPlayingAudio", Napi::Boolean::New(env, m->is_playing_audio != 0));
  out.Set("confidence", Napi::Number::New(env, m->confidence));
  out.Set("shouldRecord", Napi::Boolean::New(env, m->should_record != 0));

  Napi::Array pids = Napi::Array::New(env, m->audio_pid_count);
  for (size_t i = 0; i < m->audio_pid_count; i++) {
    pids.Set(i, Napi::Number::New(env, m->audio_pids[i]));
  }
  /* Kept out of the public TS type; the JS layer needs it to re-issue a start. */
  out.Set("_audioPids", pids);
  return out;
}

/* Read a JS meeting object back into the C struct. */
bool MeetingFromJS(const Napi::Object &obj, mrec_meeting *out) {
  std::memset(out, 0, sizeof *out);
  if (!obj.Has("platformCode") || !obj.Has("pid")) return false;
  out->platform =
      static_cast<mrec_platform>(obj.Get("platformCode").As<Napi::Number>().Int32Value());
  out->pid = obj.Get("pid").As<Napi::Number>().Uint32Value();

  if (obj.Has("_audioPids")) {
    Napi::Array pids = obj.Get("_audioPids").As<Napi::Array>();
    const size_t count = std::min<size_t>(pids.Length(), MREC_MAX_AUDIO_PIDS);
    for (size_t i = 0; i < count; i++) {
      out->audio_pids[i] = pids.Get(i).As<Napi::Number>().Uint32Value();
    }
    out->audio_pid_count = count;
  }
  return true;
}

/* ---- meeting watching ------------------------------------------------- */

Napi::ThreadSafeFunction g_meeting_tsfn;
std::atomic<bool> g_watching{false};

/* Runs on the library's serial queue — not realtime, so allocation is fine. */
void OnMeeting(const mrec_meeting *meeting, mrec_event event, void *user_data) {
  if (!g_watching.load(std::memory_order_relaxed)) return;
  auto *copy = new mrec_meeting(*meeting);
  const int32_t kind = event;

  g_meeting_tsfn.NonBlockingCall(
      copy, [kind](Napi::Env env, Napi::Function callback, mrec_meeting *m) {
        std::unique_ptr<mrec_meeting> owned(m);
        static const char *kNames[] = {"started", "updated", "ended"};
        const char *name = (kind >= 0 && kind <= 2) ? kNames[kind] : "updated";
        callback.Call({Napi::String::New(env, name), MeetingToJS(env, owned.get())});
      });
}

/* ---- exported functions ----------------------------------------------- */

Napi::Value AudioPermissionStatus(const Napi::CallbackInfo &info) {
  static const char *kNames[] = {"unknown", "granted", "denied", "not-required"};
  const int status = mrec_audio_permission_status();
  return Napi::String::New(info.Env(),
                           (status >= 0 && status <= 3) ? kNames[status] : "unknown");
}

Napi::Value RequestAudioPermission(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), mrec_request_audio_permission());
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
  static mrec_meeting meetings[16];
  size_t count = 0;
  if (mrec_scan(meetings, 16, &count) < 0 && count == 0) {
    return Napi::Array::New(env, 0);
  }
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; i++) out.Set(i, MeetingToJS(env, &meetings[i]));
  return out;
}

Napi::Value WatchStart(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (g_watching.load()) return Napi::Number::New(env, MREC_ERR_ALREADY_RUNNING);

  g_meeting_tsfn = Napi::ThreadSafeFunction::New(
      env, info[0].As<Napi::Function>(), "mrec-meetings", 0, 1);
  g_watching.store(true);

  const int32_t status = mrec_watch_start(OnMeeting, nullptr);
  if (status != MREC_OK) {
    g_watching.store(false);
    g_meeting_tsfn.Release();
  }
  return Napi::Number::New(env, status);
}

Napi::Value WatchStop(const Napi::CallbackInfo &info) {
  const int32_t status = mrec_watch_stop();
  if (g_watching.exchange(false)) g_meeting_tsfn.Release();
  return Napi::Number::New(info.Env(), status);
}

Napi::Value IsWatching(const Napi::CallbackInfo &info) {
  return Napi::Boolean::New(info.Env(), mrec_is_watching() != 0);
}

Napi::Value CaptureStart(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object opts = info[0].As<Napi::Object>();
  Napi::Function on_data = info[1].As<Napi::Function>();

  if (mrec_is_running()) {
    Napi::Error err = Napi::Error::New(env, "capture already running");
    err.Set("code", Napi::Number::New(env, MREC_ERR_ALREADY_RUNNING));
    err.ThrowAsJavaScriptException();
    return env.Undefined();
  }

  g_capture = std::make_unique<CaptureContext>();
  g_capture->tsfn =
      Napi::ThreadSafeFunction::New(env, on_data, "mrec-audio", 0, 1);
  g_capture->active.store(true);

  std::vector<uint32_t> pids;
  bool have_meeting = false;
  mrec_meeting meeting{};

  if (opts.Has("meeting") && opts.Get("meeting").IsObject()) {
    have_meeting = MeetingFromJS(opts.Get("meeting").As<Napi::Object>(), &meeting);
  } else if (opts.Has("pids") && opts.Get("pids").IsArray()) {
    Napi::Array array = opts.Get("pids").As<Napi::Array>();
    for (uint32_t i = 0; i < array.Length(); i++) {
      pids.push_back(array.Get(i).As<Napi::Number>().Uint32Value());
    }
  }
  const bool system_wide =
      opts.Has("systemWide") && opts.Get("systemWide").ToBoolean().Value();
  const bool mono = !opts.Has("mono") || opts.Get("mono").ToBoolean().Value();

  auto *worker =
      new StartWorker(env, std::move(pids), system_wide, mono, &meeting, have_meeting);
  Napi::Promise promise = worker->Promise();
  worker->Queue();
  return promise;
}

Napi::Value CaptureStop(const Napi::CallbackInfo &info) {
  const int32_t status = mrec_stop();
  if (g_capture) {
    g_capture->active.store(false);
    g_capture->tsfn.Release();
    g_capture.reset();
  }
  return Napi::Number::New(info.Env(), status);
}

Napi::Value IsRunning(const Napi::CallbackInfo &info) {
  return Napi::Boolean::New(info.Env(), mrec_is_running() != 0);
}

Napi::Value ListAudioProcesses(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  static mrec_process procs[256];
  size_t count = 0;
  mrec_list_audio_processes(procs, 256, &count);

  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; i++) {
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
  exports.Set("isRunning", Napi::Function::New(env, IsRunning));
  exports.Set("listAudioProcesses", Napi::Function::New(env, ListAudioProcesses));
  return exports;
}

NODE_API_MODULE(meeting_record, Init)
