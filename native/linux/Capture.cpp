#include "CaptureClock.h"
#include "Pulse.h"
#include "TimelineMixer.h"
#include "tracks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace meeting_record::linux_backend;
namespace {
class Session {
  struct Output {
    mrec_audio_callback callback = nullptr;
    void *user_data = nullptr;
  } system_output_, mic_output_;
  struct Monitor {
    pa_stream *stream = nullptr;
    Session *session = nullptr;
    size_t slot = 0;
    uint32_t index = PA_INVALID_INDEX, device = PA_INVALID_INDEX, pid = 0;
    uint64_t birth = 0;
    PacketClock clock;
    uint64_t total_frames = 0;
    bool microphone = false;
    std::string name;
    ~Monitor() {
      if (std::getenv("MREC_DEBUG_AUDIO"))
        std::fprintf(stderr, "monitor closed mic=%d pid=%u frames=%llu\n",
                     microphone, pid, (unsigned long long)total_frames);
      if (stream) {
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
      }
    }
  };

public:
  ~Session() { Stop(); }
  int Start(const uint32_t *pids, size_t count, bool global, bool mono,
            int microphone, mrec_audio_callback system_cb, void *system_data,
            mrec_audio_callback mic_cb, void *mic_data,
            mrec_track_event_callback event_cb, void *event_data) {
    std::lock_guard<std::mutex> lock(control_);
    if (running_)
      return Fail(MREC_ERR_ALREADY_RUNNING, "capture is already running");
    stop_ = false;
    if ((count && !pids) || (!count && !global) || !system_cb ||
        microphone < MREC_MICROPHONE_NONE ||
        microphone > MREC_MICROPHONE_AUTO || (microphone && !mic_cb))
      return Fail(MREC_ERR_INTERNAL, "invalid capture configuration");
    roots_.clear();
    for (size_t i = 0; i < count; ++i) {
      const auto process = ReadProcess(pids[i]);
      if (!process.pid)
        continue;
      if (std::none_of(roots_.begin(), roots_.end(),
                       [&](const auto &r) { return r.pid == process.pid; }))
        roots_.push_back(process);
    }
    if (count && roots_.empty())
      return Fail(MREC_ERR_NO_PROCESSES,
                  "none of the requested pids are running");
    // Drop roots contained in another root to avoid duplicate
    // capture/availability.
    const auto requested = roots_;
    roots_.erase(std::remove_if(roots_.begin(), roots_.end(),
                                [&](const auto &root) {
                                  return std::any_of(
                                      requested.begin(), requested.end(),
                                      [&](const auto &other) {
                                        return root.pid != other.pid &&
                                               Descendant(root.pid, other.pid);
                                      });
                                }),
                 roots_.end());
    pulse_ = std::make_unique<Pulse>(&stop_);
    if (!pulse_->Open()) {
      const auto error = pulse_->Error();
      const auto status = pulse_->FailureStatus();
      pulse_.reset();
      return Fail(status,
                  "cannot connect to PulseAudio or pipewire-pulse: " + error);
    }
    Snapshot snapshot;
    if (!pulse_->Read(snapshot) || snapshot.sinks.empty()) {
      pulse_.reset();
      return Fail(MREC_ERR_DEVICE_FAILED, "no audio output is available");
    }
    channels_ = mono ? 1 : 2;
    microphone_ = microphone;
    terminal_exit_ = false;
    terminal_flushed_ = false;
    // Pulse servers can batch fragments from independent sinks. Keep 200ms of
    // alignment headroom rather than committing a block before a sibling
    // arrives.
    system_output_ = {system_cb, system_data};
    mic_output_ = {mic_cb, mic_data};
    system_.Reset(1, channels_, Publish, &system_output_,
                  20); // slot 0 drives idle silence
    mic_mixer_.Reset(1, 1, Publish, &mic_output_);
    if (microphone && !FindMicrophone(snapshot)) {
      pulse_.reset();
      return Fail(MREC_ERR_DEVICE_FAILED, "no default microphone is available");
    }
    track_session_ = mrec_tracks_open(event_cb, event_data);
    const auto sources = roots_.empty() ? 1 : roots_.size();
    mrec_track_started(track_session_, MREC_TRACK_SYSTEM_AUDIO, sources * 2,
                       nullptr);
    if (microphone)
      mrec_track_started(track_session_, MREC_TRACK_MICROPHONE, 1, "");
    running_ = true;
    stop_ = false;
    Update(snapshot);
    if (terminal_exit_) {
      Cleanup();
      running_ = false;
      return Fail(MREC_ERR_NO_PROCESSES,
                  "requested apps exited during capture startup");
    }
    // Startup must report failure rather than silently recording only one
    // track.
    if ((microphone && !mic_) || monitors_.size() != wanted_) {
      const auto error = pulse_->Error();
      const auto status = pulse_->FailureStatus();
      Cleanup();
      running_ = false;
      return Fail(status, "failed to attach requested audio sources: " + error);
    }
    try {
      worker_ = std::thread([this] { Run(); });
    } catch (...) {
      Cleanup();
      running_ = false;
      return Fail(MREC_ERR_INTERNAL, "failed to create capture worker");
    }
    return MREC_OK;
  }
  int Stop() {
    std::unique_lock<std::mutex> lock(control_);
    if (!running_)
      return MREC_ERR_NOT_RUNNING;
    stop_ = true;
    wake_.notify_all();
    if (worker_.joinable())
      worker_.join();
    running_ = false;
    return MREC_OK;
  }
  bool Running() const { return running_; }
  uint32_t Channels() const { return channels_; }
  bool HasMic() const { return microphone_ != 0; }

private:
  bool RootAlive(const Process &root) const {
    const auto current = ReadProcess(root.pid);
    return current.pid && current.birth == root.birth;
  }
  bool Wanted(const AudioStream &stream) const {
    if (stream.input || !stream.pid)
      return false;
    if (roots_.empty())
      return !Descendant(stream.pid, uint32_t(getpid()));
    if (terminal_exit_)
      return false;
    return std::any_of(roots_.begin(), roots_.end(), [&](const auto &root) {
      return RootAlive(root) && Descendant(stream.pid, root.pid);
    });
  }
  const AudioDevice *
  FindMicrophone(const Snapshot &snapshot,
                 const std::string *default_source = nullptr) const {
    // AUTO prefers a source used by this app instance; otherwise use the server
    // default.
    if (microphone_ == MREC_MICROPHONE_AUTO && !roots_.empty()) {
      for (const auto &stream : snapshot.streams) {
        if (!stream.input || !stream.active || !stream.pid ||
            stream.pid == uint32_t(getpid()))
          continue;
        for (const auto &root : roots_) {
          if (!RootAlive(root) || !Descendant(stream.pid, root.pid))
            continue;
          for (const auto &source : snapshot.sources)
            if (source.index == stream.device)
              return &source;
        }
      }
    }
    for (const auto &source : snapshot.sources)
      if (source.name ==
          (default_source ? *default_source : snapshot.default_source))
        return &source;
    return nullptr;
  }
  std::unique_ptr<Monitor> Attach(const std::string &source, uint32_t index,
                                  uint32_t device, uint32_t pid,
                                  bool microphone) {
    auto monitor = std::make_unique<Monitor>();
    monitor->session = this;
    monitor->index = index;
    monitor->device = device;
    monitor->pid = pid;
    monitor->birth = ReadProcess(pid).birth;
    monitor->microphone = microphone;
    monitor->name = source;
    monitor->slot = microphone ? 0 : system_.AddSource();
    const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, 48000,
                                uint8_t(microphone ? 1 : channels_)};
    monitor->stream = pa_stream_new(
        pulse_->context, microphone ? "microphone" : "application monitor",
        &format, nullptr);
    if (!monitor->stream)
      return nullptr;
    if (!microphone && pa_stream_set_monitor_stream(monitor->stream, index) < 0)
      return nullptr;
    pa_stream_set_read_callback(monitor->stream, Read, monitor.get());
    pa_buffer_attr attr{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                        uint32_t(480 * format.channels * sizeof(float))};
    const auto flags =
        pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
    if (pa_stream_connect_record(monitor->stream, source.c_str(), &attr,
                                 flags) < 0 ||
        !pulse_->Ready(monitor->stream)) {
      if (std::getenv("MREC_DEBUG_AUDIO"))
        std::fprintf(stderr, "attach failed source=%s microphone=%d error=%s\n",
                     source.c_str(), microphone, pulse_->Error().c_str());
      return nullptr;
    }
    return monitor;
  }
  static void Read(pa_stream *stream, size_t, void *context) {
    auto &monitor = *static_cast<Monitor *>(context);
    const auto format = pa_stream_get_sample_spec(stream);
    if (!format)
      return;
    // One notification can cover several server fragments. Drain all of them;
    // consuming only one leaves a growing backlog and loses the tail at stop.
    while (pa_stream_readable_size(stream) != size_t(-1) &&
           pa_stream_readable_size(stream) > 0) {
      if (monitor.session->stop_.load()) {
        // Pacing can let the server refill a read callback continuously.
        // Stop accepting new PCM; Cleanup still drains the accepted mixer tail.
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        return;
      }
      const void *data = nullptr;
      size_t size = 0;
      if (pa_stream_peek(stream, &data, &size) < 0 || size == 0)
        return;
      const auto frames = uint32_t(size / (sizeof(float) * format->channels));
      const bool first_read = monitor.total_frames == 0;
      monitor.total_frames += frames;
      const uint64_t now = NowNs();
      const uint64_t duration = uint64_t(frames) * 1000000000ull / 48000;
      const auto readable = pa_stream_readable_size(stream);
      const uint64_t queued_frames =
          readable == size_t(-1)
              ? frames
              : readable / (sizeof(float) * format->channels);
      const uint64_t queued_ns = queued_frames * 1000000000ull / 48000;
      const uint64_t time = monitor.clock.Stamp(now, duration, queued_ns);
      if (first_read && std::getenv("MREC_DEBUG_AUDIO"))
        std::fprintf(stderr,
                     "first packet microphone=%d slot=%zu pid=%u frames=%u "
                     "hole=%d age_ms=%lld\n",
                     monitor.microphone, monitor.slot, monitor.pid, frames,
                     !data, (long long)(now - time) / 1000000);
      auto &mixer = monitor.microphone ? monitor.session->mic_mixer_
                                       : monitor.session->system_;
      mixer.Add(monitor.slot, static_cast<const float *>(data), frames, !data,
                false, time);
      pa_stream_drop(stream);
    }
  }
  void Update(const Snapshot &snapshot) {
    active_output_ = false;
    system_available_ = false;
    std::vector<bool> alive;
    for (const auto &root : roots_)
      alive.push_back(RootAlive(root));
    if (!roots_.empty() && std::none_of(alive.begin(), alive.end(),
                                        [](bool value) { return value; }))
      terminal_exit_ = true;
    std::set<uint32_t> wanted;
    for (const auto &stream : snapshot.streams) {
      if (!Wanted(stream))
        continue;
      active_output_ |= stream.active;
      wanted.insert(stream.index);
      const auto existing = monitors_.find(stream.index);
      if (existing != monitors_.end() &&
          existing->second->device == stream.device &&
          existing->second->pid == stream.pid &&
          existing->second->birth == ReadProcess(stream.pid).birth &&
          pa_stream_get_state(existing->second->stream) == PA_STREAM_READY)
        continue;
      monitors_.erase(stream.index);
      for (const auto &sink : snapshot.sinks) {
        if (sink.index != stream.device)
          continue;
        const auto source = std::to_string(sink.monitor);
        auto monitor =
            Attach(source, stream.index, stream.device, stream.pid, false);
        if (monitor)
          monitors_.emplace(stream.index, std::move(monitor));
      }
    }
    wanted_ = wanted.size();
    for (auto it = monitors_.begin(); it != monitors_.end();) {
      if (!wanted.count(it->first))
        it = monitors_.erase(it);
      else
        ++it;
    }
    // Release the captured tail before reporting terminal app exit. Holding it
    // until session stop would deliver old system audio after a continuing mic.
    if (terminal_exit_ && !terminal_flushed_) {
      system_.Flush();
      terminal_flushed_ = true;
      mrec_track_interrupted(track_session_, MREC_TRACK_SYSTEM_AUDIO,
                             MREC_TRACK_WHOLE, MREC_TRACK_REASON_APP_EXITED,
                             "captured apps exited");
    }
    const size_t source_count = roots_.empty() ? 1 : roots_.size();
    for (size_t i = 0; !terminal_exit_ && i < source_count; ++i) {
      const bool exited = !roots_.empty() && !alive[i];
      bool all = !exited && !snapshot.sinks.empty();
      bool any = all;
      size_t wanted_count = 0, attached_count = 0;
      // Existing app streams that cannot be monitored must not look like live
      // capture.
      for (const auto &stream : snapshot.streams) {
        if (!Wanted(stream) ||
            (!roots_.empty() && !Descendant(stream.pid, roots_[i].pid)))
          continue;
        ++wanted_count;
        if (monitors_.count(stream.index))
          ++attached_count;
        else
          all = false;
      }
      any = any && (!wanted_count || attached_count);
      system_available_ |= any;
      // Two availability slots per target distinguish partial stream loss
      // from losing every stream, including a single app with several helpers.
      for (uint32_t slot = 0; slot < 2; ++slot) {
        if (slot == 0 ? any : all)
          mrec_track_resumed(track_session_, MREC_TRACK_SYSTEM_AUDIO,
                             uint32_t(i * 2 + slot), nullptr);
        else
          mrec_track_interrupted(
              track_session_, MREC_TRACK_SYSTEM_AUDIO, uint32_t(i * 2 + slot),
              exited ? MREC_TRACK_REASON_APP_EXITED
                     : MREC_TRACK_REASON_DEVICE_REMOVED,
              exited ? "captured app exited" : "audio output unavailable");
      }
    }
    if (microphone_) {
      auto source = FindMicrophone(snapshot);
      if (!source && !snapshot.sources.empty() && !stop_) {
        // Older pipewire-pulse can retain a removed-source fallback for an
        // existing client while a fresh client sees the new desktop default.
        // Recheck through a control connection without interrupting playback.
        Pulse control(&stop_);
        std::string current_default;
        if (control.Open() && control.DefaultSource(current_default))
          source = FindMicrophone(snapshot, &current_default);
      }
      if (!source && std::getenv("MREC_DEBUG_AUDIO")) {
        std::fprintf(stderr, "microphone lookup default=%s sources=%zu\n",
                     snapshot.default_source.c_str(), snapshot.sources.size());
        for (const auto &item : snapshot.sources)
          std::fprintf(stderr, "  source %u %s\n", item.index,
                       item.name.c_str());
      }
      if (!source || !mic_ || mic_->name != source->name ||
          pa_stream_get_state(mic_->stream) != PA_STREAM_READY) {
        mic_.reset();
        if (source)
          mic_ = Attach(source->name, PA_INVALID_INDEX, source->index, 0, true);
        if (mic_) {
          mrec_track_resumed(track_session_, MREC_TRACK_MICROPHONE, 0,
                             source->description.c_str());
        } else
          mrec_track_interrupted(track_session_, MREC_TRACK_MICROPHONE, 0,
                                 MREC_TRACK_REASON_DEVICE_REMOVED,
                                 "microphone unavailable");
      }
    }
  }
  void Run() {
    uint64_t next_scan = 0, next_silence = NowNs();
    while (!stop_) {
      if (!pulse_->Good()) {
        monitors_.clear();
        mic_.reset();
        mrec_track_interrupted(
            track_session_, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
            MREC_TRACK_REASON_SERVICE_RESET, "audio server disconnected");
        if (microphone_)
          mrec_track_interrupted(track_session_, MREC_TRACK_MICROPHONE, 0,
                                 MREC_TRACK_REASON_SERVICE_RESET,
                                 "audio server disconnected");
        if (!pulse_->Open()) {
          std::unique_lock<std::mutex> lock(wait_lock_);
          wake_.wait_for(lock, std::chrono::milliseconds(500),
                         [this] { return stop_.load(); });
          continue;
        }
        // Clear whole-track loss before publishing per-target availability.
        if (!terminal_exit_)
          mrec_track_resumed(track_session_, MREC_TRACK_SYSTEM_AUDIO,
                             MREC_TRACK_WHOLE, nullptr);
        next_scan = 0;
      }
      pulse_->Iterate();
      const auto now = NowNs();
      if (now >= next_scan) {
        Snapshot snapshot;
        if (pulse_->Read(snapshot))
          Update(snapshot);
        else if (std::getenv("MREC_DEBUG_AUDIO"))
          std::fprintf(stderr, "graph read failed: %s\n",
                       pulse_->Error().c_str());
        next_scan = NowNs() + 250000000;
      }
      if (now >= next_silence) {
        // A 40ms watermark allows late real packets to reach the shared mixer.
        // App exit is terminal for the system track, even if the PID is reused.
        if (!terminal_exit_ && system_available_ && !active_output_)
          system_.Add(0, nullptr, 480, true, false, now - 40000000);
        next_silence = now + 10000000;
      }
      std::unique_lock<std::mutex> lock(wait_lock_);
      wake_.wait_for(lock, std::chrono::milliseconds(2),
                     [this] { return stop_.load(); });
    }
    Cleanup();
  }
  static void AwaitSamples(uint64_t end_ns) {
    const auto now = NowNs();
    if (end_ns > now) {
      // A server can deliver render batches ahead of the sample clock. Pace
      // publication, including final drain, instead of publishing future
      // timestamps or losing PCM.
      std::this_thread::sleep_for(std::chrono::nanoseconds(end_ns - now));
    }
  }
  static void Publish(const float *frames, uint32_t frame_count,
                      uint32_t channels, double sample_rate,
                      uint64_t host_time_ns, void *context) {
    auto &output = *static_cast<Output *>(context);
    AwaitSamples(host_time_ns);
    if (output.callback)
      output.callback(frames, frame_count, channels, sample_rate, host_time_ns,
                      output.user_data);
  }
  void Cleanup() {
    monitors_.clear();
    mic_.reset();
    system_.Flush();
    mic_mixer_.Flush();
    pulse_.reset();
    mrec_tracks_close();
  }
  std::mutex control_, wait_lock_;
  std::condition_variable wake_;
  std::atomic<bool> running_{false}, stop_{false};
  std::thread worker_;
  std::unique_ptr<Pulse> pulse_;
  std::vector<Process> roots_;
  std::map<uint32_t, std::unique_ptr<Monitor>> monitors_;
  std::unique_ptr<Monitor> mic_;
  meeting_record::TimelineMixer system_, mic_mixer_;
  uint32_t channels_ = 1;
  int microphone_ = 0;
  bool terminal_exit_ = false;
  bool terminal_flushed_ = false;
  bool active_output_ = false;
  bool system_available_ = false;
  size_t wanted_ = 0;
  uint64_t track_session_ = 0;
};
Session &Current() {
  static Session session;
  return session;
}

mrec_permission AudioPermission() {
  Pulse pulse;
  return pulse.Open() ? MREC_PERM_GRANTED : MREC_PERM_UNKNOWN;
}
int List(mrec_process *out, size_t capacity, size_t *count, bool apps) {
  if (!count || (capacity && !out))
    return Fail(MREC_ERR_INTERNAL, "invalid process buffer");
  *count = 0;
  Pulse pulse;
  Snapshot snapshot;
  if (!pulse.Open() || !pulse.Read(snapshot))
    return Fail(pulse.FailureStatus(), pulse.Error());
  std::map<uint32_t, mrec_process> processes;
  for (const auto &stream : snapshot.streams) {
    if (!stream.pid || Descendant(stream.pid, uint32_t(getpid())))
      continue;
    const auto process = apps ? AppRoot(stream.pid) : ReadProcess(stream.pid);
    if (!process.pid)
      continue;
    auto &entry = processes[process.pid];
    entry.pid = process.pid;
    Copy(entry.name, process.name);
    if (stream.input)
      entry.is_running_input |= stream.active;
    else
      entry.is_running_output |= stream.active;
  }
  for (const auto &[pid, process] : processes) {
    if (*count < capacity)
      out[(*count)++] = process;
  }
  return processes.size() > capacity ? MREC_ERR_BUFFER_TOO_SMALL : MREC_OK;
}
} // namespace

extern "C" {
mrec_permission mrec_audio_permission_status() { return AudioPermission(); }
mrec_permission mrec_microphone_permission_status() {
  return AudioPermission();
}
mrec_status mrec_request_audio_permission() { return MREC_OK; }
mrec_status mrec_request_microphone_permission() { return MREC_OK; }
mrec_status mrec_list_audio_processes(mrec_process *out, size_t cap,
                                      size_t *count) {
  return mrec_status(List(out, cap, count, false));
}
mrec_status mrec_list_audio_apps(mrec_process *out, size_t cap, size_t *count) {
  return mrec_status(List(out, cap, count, true));
}
int32_t mrec_start_tracks_raw(const uint32_t *pids, size_t count,
                              int32_t global, int32_t mono, int32_t,
                              int32_t microphone, mrec_audio_callback system_cb,
                              void *system_data, mrec_audio_callback mic_cb,
                              void *mic_data,
                              mrec_track_event_callback event_cb,
                              void *event_data) {
  return Current().Start(pids, count, global, mono, microphone, system_cb,
                         system_data, mic_cb, mic_data, event_cb, event_data);
}
int32_t mrec_start_raw(const uint32_t *pids, size_t count, int32_t global,
                       int32_t mono, int32_t mute, mrec_audio_callback cb,
                       void *data) {
  return mrec_start_tracks_raw(pids, count, global, mono, mute, 0, cb, data,
                               nullptr, nullptr, nullptr, nullptr);
}
mrec_status mrec_stop() { return mrec_status(Current().Stop()); }
int32_t mrec_is_running() { return Current().Running(); }
mrec_status mrec_current_format(double *rate, uint32_t *channels) {
  if (!Current().Running())
    return MREC_ERR_NOT_RUNNING;
  if (rate)
    *rate = 48000;
  if (channels)
    *channels = Current().Channels();
  return MREC_OK;
}
mrec_status mrec_current_microphone_format(double *rate, uint32_t *channels) {
  if (!Current().Running() || !Current().HasMic())
    return MREC_ERR_NOT_RUNNING;
  if (rate)
    *rate = 48000;
  if (channels)
    *channels = 1;
  return MREC_OK;
}
const char *mrec_last_error() { return last_error.c_str(); }
}
