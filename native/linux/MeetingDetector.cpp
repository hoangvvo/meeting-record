#include "Accessibility.h"
#include "Pulse.h"
#include "meeting-catalog.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>

using namespace meeting_record::linux_backend;
namespace {
bool Scan(std::vector<mrec_meeting> &out) {
  Pulse pulse;
  Snapshot snapshot;
  if (!pulse.Open() || !pulse.Read(snapshot)) {
    Fail(pulse.FailureStatus(), pulse.Error());
    return false;
  }
  const auto evidence = ReadAccessibility();
  struct Activity {
    Process root;
    bool output = false, mic = false;
    std::set<uint32_t> pids;
  };
  std::map<uint32_t, Activity> apps;
  for (const auto &stream : snapshot.streams) {
    if (!stream.pid || Descendant(stream.pid, uint32_t(getpid())))
      continue;
    auto root = AppRoot(stream.pid);
    if (!root.pid)
      continue;
    auto &app = apps[root.pid];
    app.root = root;
    if (stream.input) {
      // Monitor sources are not microphones. Otherwise our own monitors would
      // make unrelated playback look like a call.
      const bool microphone =
          std::any_of(snapshot.sources.begin(), snapshot.sources.end(),
                      [&](const auto &s) { return s.index == stream.device; });
      app.mic |= microphone && stream.active;
    } else {
      app.output |= stream.active;
      app.pids.insert(stream.pid);
    }
  }
  // Accessible joined controls can detect silent/listen-only calls with no
  // audio streams.
  for (const auto &[pid, entry] : evidence)
    if (!apps.count(pid))
      apps[pid].root = ReadProcess(pid);
  for (const auto &[pid, app] : apps) {
    const auto platform = mrec_catalog_linux_platform(app.root.name.c_str());
    const bool browser =
        mrec_catalog_linux_browser(app.root.name.c_str()) != nullptr;
    if (!platform && !browser)
      continue;
    WindowEvidence selected;
    selected.platform = platform;
    int best = -1;
    const auto found = evidence.find(pid);
    if (found != evidence.end()) {
      for (const auto &window : found->second.windows) {
        // General service context (e.g. Teams /v2) scopes controls, but does
        // not satisfy the stricter browser URL/title fallback by itself.
        const bool recognised =
            mrec_catalog_url_platform(window.url.c_str()) ||
            mrec_catalog_title_platform(window.title.c_str(), nullptr);
        const auto confidence = mrec_catalog_call_confidence(
            browser, app.output, app.mic, recognised ? window.platform : 0,
            window.call_state);
        if (confidence > best) {
          best = confidence;
          selected = window;
        }
      }
    }
    if (best < 0)
      best = mrec_catalog_call_confidence(browser, app.output, app.mic, 0,
                                          MREC_CALL_UNKNOWN);
    if (!best || (browser && !selected.platform))
      continue;
    mrec_meeting meeting{};
    meeting.platform = mrec_platform(selected.platform);
    meeting.pid = pid;
    Copy(meeting.app_name, app.root.name);
    Copy(meeting.title, selected.title);
    Copy(meeting.url, selected.url);
    meeting.is_using_mic = app.mic;
    meeting.is_playing_audio = app.output;
    meeting.confidence = best;
    meeting.should_record = best >= MREC_CATALOG_RECORD_THRESHOLD;
    meeting.detected_at_ns = NowNs();
    for (auto audio_pid : app.pids) {
      if (meeting.audio_pid_count < MREC_MAX_AUDIO_PIDS)
        meeting.audio_pids[meeting.audio_pid_count++] = audio_pid;
    }
    if (!meeting.audio_pid_count)
      meeting.audio_pids[meeting.audio_pid_count++] = pid;
    out.push_back(meeting);
  }
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return a.confidence > b.confidence;
  });
  return true;
}

class Watcher {
  struct Run {
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable wake;
    mrec_meeting_callback callback;
    void *data;
  };
  struct Tracked {
    mrec_meeting meeting{};
    uint64_t birth = 0, first = 0, last = 0;
    bool started = false;
  };

public:
  ~Watcher() { Stop(); }
  int Start(mrec_meeting_callback callback, void *data) {
    std::lock_guard<std::mutex> lock(control_);
    if (run_)
      return MREC_ERR_ALREADY_RUNNING;
    if (!callback)
      return MREC_ERR_INTERNAL;
    auto run = std::make_shared<Run>();
    run->callback = callback;
    run->data = data;
    try {
      thread_ = std::thread([run] { Loop(run); });
    } catch (...) {
      return Fail(MREC_ERR_INTERNAL, "failed to create meeting watcher");
    }
    run_ = run;
    return MREC_OK;
  }
  int Stop() {
    std::unique_lock<std::mutex> lock(control_);
    if (!run_)
      return MREC_ERR_NOT_RUNNING;
    run_->stop = true;
    run_->wake.notify_all();
    std::thread stopping = std::move(thread_);
    run_.reset();
    lock.unlock();
    if (stopping.get_id() == std::this_thread::get_id())
      stopping.detach();
    else
      stopping.join();
    return MREC_OK;
  }
  bool Running() {
    std::lock_guard<std::mutex> lock(control_);
    return bool(run_);
  }

private:
  static bool Changed(mrec_meeting a, mrec_meeting b) {
    return a.platform != b.platform || a.pid != b.pid ||
           a.confidence != b.confidence || a.should_record != b.should_record ||
           a.is_using_mic != b.is_using_mic ||
           a.is_playing_audio != b.is_playing_audio ||
           a.audio_pid_count != b.audio_pid_count ||
           std::strcmp(a.title, b.title) || std::strcmp(a.url, b.url) ||
           std::strcmp(a.app_name, b.app_name) ||
           !std::equal(a.audio_pids, a.audio_pids + a.audio_pid_count,
                       b.audio_pids);
  }
  static void Loop(const std::shared_ptr<Run> &run) {
    std::map<uint32_t, Tracked> tracked;
    while (!run->stop) {
      std::vector<mrec_meeting> found;
      const auto scan_started = NowNs();
      const bool scanned = Scan(found);
      if (std::getenv("MREC_DEBUG_WATCH"))
        std::fprintf(stderr, "watch scan ok=%d found=%zu duration_ms=%llu\n",
                     scanned, found.size(),
                     (unsigned long long)(NowNs() - scan_started) / 1000000);
      if (scanned && !run->stop) {
        const auto now = NowNs();
        for (const auto &meeting : found) {
          if (run->stop)
            return;
          if (!meeting.should_record)
            continue;
          auto &entry = tracked[meeting.pid];
          const auto birth = ReadProcess(meeting.pid).birth;
          if (entry.birth && entry.birth != birth) {
            if (entry.started)
              run->callback(&entry.meeting, MREC_MEETING_ENDED, run->data);
            entry = {};
            if (run->stop)
              return;
          }
          if (!entry.first) {
            entry.first = now;
            entry.birth = birth;
          }
          entry.last = now;
          const bool changed = Changed(entry.meeting, meeting);
          entry.meeting = meeting;
          if (!entry.started &&
              now - entry.first >= MREC_CATALOG_START_DELAY_MS * 1000000ull) {
            entry.started = true;
            run->callback(&entry.meeting, MREC_MEETING_STARTED, run->data);
          } else if (entry.started && changed)
            run->callback(&entry.meeting, MREC_MEETING_UPDATED, run->data);
        }
        for (auto it = tracked.begin(); it != tracked.end();) {
          if (run->stop)
            return;
          if (std::getenv("MREC_DEBUG_WATCH"))
            std::fprintf(stderr,
                         "watch tracked pid=%u missing_ms=%llu started=%d\n",
                         it->first,
                         (unsigned long long)(now - it->second.last) / 1000000,
                         it->second.started);
          if (it->second.last == now ||
              (it->second.started &&
               now - it->second.last <
                   MREC_CATALOG_END_DELAY_MS * 1000000ull)) {
            ++it;
            continue;
          }
          if (it->second.started)
            run->callback(&it->second.meeting, MREC_MEETING_ENDED, run->data);
          it = tracked.erase(it);
        }
      }
      std::unique_lock<std::mutex> lock(run->mutex);
      run->wake.wait_for(lock, std::chrono::milliseconds(500),
                         [&] { return run->stop.load(); });
    }
  }
  std::mutex control_;
  std::shared_ptr<Run> run_;
  std::thread thread_;
};
Watcher &CurrentWatcher() {
  static Watcher watcher;
  return watcher;
}
} // namespace

extern "C" {
mrec_status mrec_scan(mrec_meeting *out, size_t capacity, size_t *count) {
  if (!count || (capacity && !out))
    return MREC_ERR_INTERNAL;
  *count = 0;
  std::vector<mrec_meeting> found;
  if (!Scan(found))
    return mrec_status(last_status);
  *count = std::min(capacity, found.size());
  std::copy_n(found.begin(), *count, out);
  return found.size() > capacity ? MREC_ERR_BUFFER_TOO_SMALL : MREC_OK;
}
mrec_status mrec_start_meeting(const mrec_meeting *meeting,
                               mrec_audio_callback cb, void *data) {
  if (!meeting || !meeting->pid)
    return MREC_ERR_INTERNAL;
  // Capture the original instance and its future helpers, never another call
  // found by service name. Capture performs PID/birth validation at startup.
  return mrec_status(mrec_start_raw(&meeting->pid, 1, 0, 1, 0, cb, data));
}
mrec_status mrec_watch_start(mrec_meeting_callback cb, void *data) {
  return mrec_status(CurrentWatcher().Start(cb, data));
}
mrec_status mrec_watch_stop() { return mrec_status(CurrentWatcher().Stop()); }
int32_t mrec_is_watching() { return CurrentWatcher().Running(); }
mrec_permission mrec_accessibility_permission_status() {
  return AccessibilityAvailable() ? MREC_PERM_GRANTED : MREC_PERM_UNKNOWN;
}
mrec_status mrec_request_accessibility_permission() {
  // There is no portable Linux grant dialog. The host app/session enables
  // AT-SPI.
  return AccessibilityAvailable() ? MREC_OK : MREC_ERR_PERMISSION;
}
}
