/*
 * Windows meeting detection.
 *
 * Same layered design as the macOS backend. There is no equivalent of
 * kAudioHardwarePropertyProcessObjectList, so liveness comes from
 * IAudioSessionManager2 session enumeration: an ACTIVE render session on a
 * conferencing process is the equivalent signal.
 *
 * Window titles come from EnumWindows + GetWindowTextW, which needs no permission.
 * Full UI Automation would also yield browser URLs but is not used, so titles
 * carry the URL matching.
 */
#include "meeting-record-detect.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct NativeApp {
  const char *exe; /* lowercased executable name */
  mrec_platform platform;
};

/* Matched on executable name; Windows has no bundle identifiers. */
constexpr NativeApp kNativeApps[] = {
    {"zoom.exe", MREC_PLATFORM_ZOOM},
    {"cpthost.exe", MREC_PLATFORM_ZOOM}, /* Zoom's screen-share helper */
    {"teams.exe", MREC_PLATFORM_TEAMS},
    {"ms-teams.exe", MREC_PLATFORM_TEAMS},
    {"msteams.exe", MREC_PLATFORM_TEAMS},
    {"webexmta.exe", MREC_PLATFORM_WEBEX},
    {"webex.exe", MREC_PLATFORM_WEBEX},
    {"atmgr.exe", MREC_PLATFORM_WEBEX},
    {"slack.exe", MREC_PLATFORM_SLACK},
    {"discord.exe", MREC_PLATFORM_DISCORD},
};

constexpr const char *kBrowsers[] = {"chrome.exe", "msedge.exe", "firefox.exe",
                                     "brave.exe",  "arc.exe",    "vivaldi.exe",
                                     "opera.exe"};

struct UrlPattern {
  const char *pattern;
  mrec_platform platform;
};

constexpr UrlPattern kUrlPatterns[] = {
    {"meet.google.com", MREC_PLATFORM_MEET},
    {"teams.microsoft.com", MREC_PLATFORM_TEAMS},
    {"teams.live.com", MREC_PLATFORM_TEAMS},
    {"zoom.us/j/", MREC_PLATFORM_ZOOM},
    {"zoom.us/wc/", MREC_PLATFORM_ZOOM},
    {"app.slack.com/huddle", MREC_PLATFORM_SLACK},
    {"whereby.com/", MREC_PLATFORM_GENERIC_BROWSER},
    {"meet.jit.si", MREC_PLATFORM_GENERIC_BROWSER},
};

/* Must match the macOS backend. */
constexpr int32_t kScoreKnownApp = 40;
constexpr int32_t kScorePlayingAudio = 30;
constexpr int32_t kScoreUsingMic = 25;
constexpr int32_t kScoreRecognisedUrl = 5;
constexpr int32_t kRecordThreshold = 70;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return value;
}

std::string Narrow(const std::wstring &wide) {
  if (wide.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr,
                                 nullptr);
  if (size <= 1) return {};
  /* `size` includes the terminating NUL. Reserve room for it before trimming it
   * off; passing `size` into a `size - 1` string is a one-byte overwrite. */
  std::string out(static_cast<size_t>(size), '\0');
  const int written = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                          out.data(), size, nullptr, nullptr);
  if (written <= 1) return {};
  out.resize(static_cast<size_t>(written - 1));
  return out;
}

uint64_t MonotonicTimeNs() {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
      !QueryPerformanceCounter(&counter)) {
    return GetTickCount64() * 1000000ull;
  }
  const uint64_t whole = static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart);
  const uint64_t remainder =
      static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart);
  return whole * 1000000000ull +
         remainder * 1000000000ull / static_cast<uint64_t>(frequency.QuadPart);
}

mrec_platform NativePlatformFor(const std::string &exe_lower) {
  for (const auto &app : kNativeApps) {
    if (exe_lower == app.exe) return app.platform;
  }
  return MREC_PLATFORM_UNKNOWN;
}

bool IsBrowser(const std::string &exe_lower) {
  for (const char *browser : kBrowsers) {
    if (exe_lower == browser) return true;
  }
  return false;
}

mrec_platform PlatformForText(const std::string &text) {
  if (text.empty()) return MREC_PLATFORM_UNKNOWN;
  const std::string haystack = ToLower(text);
  for (const auto &entry : kUrlPatterns) {
    if (haystack.find(entry.pattern) != std::string::npos) return entry.platform;
  }
  return MREC_PLATFORM_UNKNOWN;
}

struct AudioActivity {
  bool rendering = false;
  bool capturing = false;
};

class ScopedComApartment {
public:
  ScopedComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ScopedComApartment() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }

private:
  HRESULT result_;
};

/* Per-process audio activity across every render and capture endpoint. */
std::map<DWORD, AudioActivity> CollectAudioActivity() {
  ScopedComApartment apartment;
  std::map<DWORD, AudioActivity> activity;

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(
                                  enumerator.GetAddressOf())))) {
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

    for (UINT d = 0; d < device_count; d++) {
      ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(d, device.GetAddressOf()))) continue;

      ComPtr<IAudioSessionManager2> manager;
      if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                 nullptr,
                                 reinterpret_cast<void **>(
                                     manager.GetAddressOf())))) {
        continue;
      }
      ComPtr<IAudioSessionEnumerator> sessions;
      if (FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf()))) continue;

      int session_count = 0;
      sessions->GetCount(&session_count);
      for (int s = 0; s < session_count; s++) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(s, control.GetAddressOf()))) continue;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(&control2))) continue;

        DWORD pid = 0;
        if (FAILED(control2->GetProcessId(&pid)) || pid == 0) continue;

        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) {
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

struct VisibleWindow {
  DWORD pid = 0;
  std::string executable;
  std::string executable_lower;
  std::string title;
  uint64_t area = 0;
};

std::string ExecutableName(DWORD pid);

BOOL CALLBACK CollectWindowProc(HWND window, LPARAM param) {
  auto *windows = reinterpret_cast<std::vector<VisibleWindow> *>(param);
  if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
  if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;

  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return TRUE;
  std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(window, buffer.data(), length + 1);
  buffer.resize(static_cast<size_t>(length));

  std::string title = Narrow(buffer);
  std::string executable = ExecutableName(pid);
  if (title.empty() || executable.empty()) return TRUE;

  RECT rect{};
  uint64_t area = 0;
  if (GetWindowRect(window, &rect) && rect.right > rect.left && rect.bottom > rect.top) {
    area = static_cast<uint64_t>(rect.right - rect.left) *
           static_cast<uint64_t>(rect.bottom - rect.top);
  }
  windows->push_back(
      {pid, executable, ToLower(executable), std::move(title), area});
  return TRUE;
}

std::vector<VisibleWindow> CollectVisibleWindows() {
  std::vector<VisibleWindow> windows;
  EnumWindows(CollectWindowProc, reinterpret_cast<LPARAM>(&windows));
  return windows;
}

std::string ExecutableName(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  wchar_t path[MAX_PATH] = {0};
  DWORD size = MAX_PATH;
  std::string name;
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    std::wstring full(path, size);
    const size_t slash = full.find_last_of(L'\\');
    name = Narrow(slash == std::wstring::npos ? full : full.substr(slash + 1));
  }
  CloseHandle(process);
  return name;
}

struct Detected {
  mrec_platform platform = MREC_PLATFORM_UNKNOWN;
  DWORD pid = 0;
  std::vector<DWORD> audio_pids;
  std::string app_name;
  std::string title;
  std::string url;
  bool mic = false;
  bool output = false;
  int32_t confidence = 0;
};

template <typename Predicate>
const VisibleWindow *BestWindow(const std::vector<VisibleWindow> &windows,
                                Predicate predicate) {
  const VisibleWindow *best = nullptr;
  for (const auto &window : windows) {
    if (!predicate(window)) continue;
    if (!best || window.area > best->area ||
        (window.area == best->area && window.title.size() > best->title.size())) {
      best = &window;
    }
  }
  return best;
}

std::vector<Detected> Scan() {
  const auto activity = CollectAudioActivity();
  const auto windows = CollectVisibleWindows();
  const DWORD self = GetCurrentProcessId();

  /* Group by platform (native) or by executable (browsers). */
  std::map<mrec_platform, Detected> native;
  std::map<std::string, Detected> browser;

  for (const auto &[pid, state] : activity) {
    if (pid == self) continue;
    if (!state.rendering && !state.capturing) continue;

    const std::string exe = ExecutableName(pid);
    if (exe.empty()) continue;
    const std::string exe_lower = ToLower(exe);

    if (const auto platform = NativePlatformFor(exe_lower);
        platform != MREC_PLATFORM_UNKNOWN) {
      Detected &entry = native[platform];
      entry.platform = platform;
      if (entry.pid == 0) entry.pid = pid;
      entry.audio_pids.push_back(pid);
      entry.app_name = exe;
      entry.mic = entry.mic || state.capturing;
      entry.output = entry.output || state.rendering;
    } else if (IsBrowser(exe_lower)) {
      Detected &entry = browser[exe_lower];
      if (entry.pid == 0) entry.pid = pid;
      entry.audio_pids.push_back(pid);
      entry.app_name = exe;
      entry.mic = entry.mic || state.capturing;
      entry.output = entry.output || state.rendering;
    }
  }

  std::vector<Detected> results;

  for (auto &[platform, entry] : native) {
    if (const auto *window = BestWindow(windows, [platform](const VisibleWindow &candidate) {
          return NativePlatformFor(candidate.executable_lower) == platform;
        })) {
      entry.pid = window->pid;
      entry.app_name = window->executable;
      entry.title = window->title;
    }
    entry.confidence = kScoreKnownApp + (entry.output ? kScorePlayingAudio : 0) +
                       (entry.mic ? kScoreUsingMic : 0);
    results.push_back(entry);
  }

  for (auto &[exe, entry] : browser) {
    const auto matching_executable = [&exe](const VisibleWindow &candidate) {
      return candidate.executable_lower == exe;
    };
    const VisibleWindow *window = BestWindow(windows, [&matching_executable](
                                                           const VisibleWindow &candidate) {
      return matching_executable(candidate) &&
             PlatformForText(candidate.title) != MREC_PLATFORM_UNKNOWN;
    });
    if (!window) window = BestWindow(windows, matching_executable);
    if (window) {
      entry.pid = window->pid;
      entry.app_name = window->executable;
      entry.title = window->title;
    }
    /*
     * A video tab and a call tab are indistinguishable at the process level, so
     * require a recognised title or a live microphone.
     */
    const mrec_platform matched = PlatformForText(entry.title);
    if (matched == MREC_PLATFORM_UNKNOWN && !entry.mic) continue;

    entry.platform =
        matched != MREC_PLATFORM_UNKNOWN ? matched : MREC_PLATFORM_GENERIC_BROWSER;
    entry.confidence = kScoreKnownApp + (entry.output ? kScorePlayingAudio : 0) +
                       (entry.mic ? kScoreUsingMic : 0) +
                       (matched != MREC_PLATFORM_UNKNOWN ? kScoreRecognisedUrl : 0);
    results.push_back(entry);
  }

  std::sort(results.begin(), results.end(),
            [](const Detected &a, const Detected &b) {
              return a.confidence > b.confidence;
            });
  return results;
}

void Fill(mrec_meeting *out, const Detected &d) {
  std::memset(out, 0, sizeof *out);
  out->platform = d.platform;
  out->pid = d.pid;
  out->audio_pid_count =
      (std::min)(d.audio_pids.size(), static_cast<size_t>(MREC_MAX_AUDIO_PIDS));
  for (size_t i = 0; i < out->audio_pid_count; i++) {
    out->audio_pids[i] = d.audio_pids[i];
  }
  std::snprintf(out->app_name, sizeof out->app_name, "%s", d.app_name.c_str());
  std::snprintf(out->title, sizeof out->title, "%s", d.title.c_str());
  std::snprintf(out->url, sizeof out->url, "%s", d.url.c_str());
  out->is_using_mic = d.mic ? 1 : 0;
  out->is_playing_audio = d.output ? 1 : 0;
  out->confidence = (std::min)(d.confidence, 100);
  out->should_record = out->confidence >= kRecordThreshold ? 1 : 0;

  out->detected_at_ns = MonotonicTimeNs();
}

class Watcher {
public:
  static Watcher &Instance() {
    static Watcher watcher;
    return watcher;
  }

  mrec_status Start(mrec_meeting_callback cb, void *user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return MREC_ERR_ALREADY_RUNNING;
    callback_ = cb;
    user_data_ = user_data;
    running_ = true;
    stop_ = false;
    const uint64_t generation =
        generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    try {
      thread_ = std::thread([this, generation] { Loop(generation); });
    } catch (...) {
      running_ = false;
      callback_ = nullptr;
      user_data_ = nullptr;
      return MREC_ERR_INTERNAL;
    }
    return MREC_OK;
  }

  mrec_status Stop() {
    bool stopping_from_callback = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) return MREC_ERR_NOT_RUNNING;
      stop_ = true;
      generation_.fetch_add(1, std::memory_order_acq_rel);
      stopping_from_callback =
          thread_.joinable() && thread_.get_id() == std::this_thread::get_id();
      if (stopping_from_callback) {
        thread_.detach();
        running_ = false;
        active_.clear();
        callback_ = nullptr;
        user_data_ = nullptr;
      }
    }
    if (stopping_from_callback) return MREC_OK;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    active_.clear();
    callback_ = nullptr;
    return MREC_OK;
  }

  bool running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

private:
  /*
   * Polled: IAudioSessionNotification fires only on session creation, not on the
   * inactive->active transition that marks a call going live.
   */
  bool ShouldStop(uint64_t generation) const {
    return stop_.load(std::memory_order_acquire) ||
           generation_.load(std::memory_order_acquire) != generation;
  }

  void Loop(uint64_t generation) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!ShouldStop(generation)) {
      Rescan(generation);
      for (int i = 0; i < 20 && !ShouldStop(generation); i++) {
        Sleep(100);
      }
    }
    CoUninitialize();
  }

  void Rescan(uint64_t generation) {
    if (ShouldStop(generation)) return;
    auto found = Scan();
    std::map<DWORD, Detected> next;
    for (auto &entry : found) next[entry.pid] = entry;

    mrec_meeting payload;
    for (const auto &[pid, entry] : next) {
      if (ShouldStop(generation)) return;
      auto previous = active_.find(pid);
      if (previous == active_.end()) {
        Fill(&payload, entry);
        callback_(&payload, MREC_MEETING_STARTED, user_data_);
      } else if (Changed(previous->second, entry)) {
        Fill(&payload, entry);
        callback_(&payload, MREC_MEETING_UPDATED, user_data_);
      }
      if (ShouldStop(generation)) return;
    }
    for (const auto &[pid, entry] : active_) {
      if (ShouldStop(generation)) return;
      if (next.find(pid) == next.end()) {
        Fill(&payload, entry);
        callback_(&payload, MREC_MEETING_ENDED, user_data_);
        if (ShouldStop(generation)) return;
      }
    }
    if (ShouldStop(generation)) return;
    active_ = std::move(next);
  }

  static bool Changed(const Detected &a, const Detected &b) {
    return a.platform != b.platform || a.title != b.title || a.mic != b.mic ||
           a.output != b.output || a.audio_pids != b.audio_pids;
  }

  std::mutex mutex_;
  bool running_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> generation_{0};
  std::thread thread_;
  std::map<DWORD, Detected> active_;
  mrec_meeting_callback callback_ = nullptr;
  void *user_data_ = nullptr;
};

} // namespace

extern "C" {

mrec_status mrec_scan(mrec_meeting *out, size_t capacity,
                                  size_t *out_count) {
  if (!out || !out_count) return MREC_ERR_INTERNAL;
  const auto found = Scan();
  const size_t count = (std::min)(found.size(), capacity);
  for (size_t i = 0; i < count; i++) Fill(&out[i], found[i]);
  *out_count = count;
  return found.size() > capacity ? MREC_ERR_BUFFER_TOO_SMALL : MREC_OK;
}

mrec_status mrec_start_meeting(const mrec_meeting *meeting,
                               mrec_audio_callback callback,
                               void *user_data) {
  if (!meeting) return MREC_ERR_INTERNAL;

  /* INCLUDE_PROCESS_TREE should start at the visible owner, not at each current
   * audio helper: that follows helpers created later and prevents double capture.
   * Refresh the owner because the caller may have held the meeting for a while. */
  uint32_t target = meeting->pid;
  const auto current = Scan();
  const auto fresh = std::find_if(current.begin(), current.end(), [meeting](const Detected &item) {
    return item.platform == meeting->platform;
  });
  if (fresh != current.end() && fresh->pid != 0) target = fresh->pid;
  if (target == 0 && meeting->audio_pid_count > 0) target = meeting->audio_pids[0];
  if (target == 0) return MREC_ERR_NO_PROCESSES;

  return static_cast<mrec_status>(
      mrec_start_raw(&target, 1, 0, 1, 0, callback, user_data));
}

mrec_status mrec_watch_start(mrec_meeting_callback cb,
                                         void *user_data) {
  if (!cb) return MREC_ERR_INTERNAL;
  return Watcher::Instance().Start(cb, user_data);
}

mrec_status mrec_watch_stop(void) {
  return Watcher::Instance().Stop();
}

int32_t mrec_is_watching(void) {
  return Watcher::Instance().running() ? 1 : 0;
}

mrec_permission mrec_accessibility_permission_status(void) {
  /* UI Automation and EnumWindows need no grant on Windows. */
  return MREC_PERM_NOT_REQUIRED;
}

mrec_status mrec_request_accessibility_permission(void) { return MREC_OK; }

const char *mrec_platform_id(mrec_platform platform) {
  switch (platform) {
    case MREC_PLATFORM_ZOOM: return "zoom";
    case MREC_PLATFORM_TEAMS: return "teams";
    case MREC_PLATFORM_MEET: return "meet";
    case MREC_PLATFORM_WEBEX: return "webex";
    case MREC_PLATFORM_SLACK: return "slack";
    case MREC_PLATFORM_DISCORD: return "discord";
    case MREC_PLATFORM_GENERIC_BROWSER: return "browser";
    case MREC_PLATFORM_UNKNOWN: break;
  }
  return "unknown";
}

} // extern "C"
