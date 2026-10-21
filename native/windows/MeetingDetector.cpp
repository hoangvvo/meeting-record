/*
 * Windows meeting detection.
 *
 * Same layered design as the macOS backend. There is no equivalent of
 * kAudioHardwarePropertyProcessObjectList, so liveness comes from
 * IAudioSessionManager2 session enumeration: an ACTIVE render session on a
 * conferencing process is the equivalent signal.
 *
 * Window titles come from EnumWindows + GetWindowTextW, and browser URLs and tab
 * titles from UI Automation. Neither needs a permission.
 */
#include "meeting-record-detect.h"
#include "AudioAppProcess.h"
#include "meeting-catalog.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <tlhelp32.h>
#include <uiautomation.h>

using Microsoft::WRL::ComPtr;

namespace {

mrec_platform AppPlatform(const std::string &executable) {
  return static_cast<mrec_platform>(mrec_catalog_executable_platform(executable.c_str()));
}

bool IsBrowser(const std::string &executable) {
  return mrec_catalog_executable_browser(executable.c_str()) != nullptr;
}

mrec_platform UrlPlatform(const std::string &url) {
  return static_cast<mrec_platform>(mrec_catalog_url_platform(url.c_str()));
}

/* The meeting `title` names; narrows it to the page title. */
mrec_platform TitlePlatform(std::string &title) {
  size_t length = title.size();
  const auto platform =
      static_cast<mrec_platform>(mrec_catalog_title_platform(title.c_str(), &length));
  title.resize(length);
  return platform;
}

/* UI Automation bounds, as for macOS Accessibility: each call is synchronous IPC. */
constexpr DWORD kAccessibilityTimeoutMs = 250;
constexpr uint64_t kAccessibilityTraversalNs = 500000000;
constexpr int kAccessibilityMaxDepth = 24;
constexpr int kAccessibilityMaxNodes = 600;
constexpr int kMaxBrowserWindows = 3;

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
  HWND handle = nullptr;
  DWORD pid = 0;
  std::string executable;
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
      {window, pid, executable, std::move(title), area});
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

/* Every process, marked when it owns one of `windows`, for FindAudioAppRoot. */
std::map<uint32_t, meeting_record::AudioAppProcess> CollectProcesses(
    const std::vector<VisibleWindow> &windows) {
  std::map<uint32_t, meeting_record::AudioAppProcess> processes;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return processes;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      processes.emplace(entry.th32ProcessID, meeting_record::AudioAppProcess{
          entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  for (const auto &window : windows) {
    const auto process = processes.find(window.pid);
    if (process != processes.end()) process->second.has_app_window = true;
  }
  return processes;
}

/* What a browser window shows: its active tab's address and every tab's title. */
struct BrowserWindowContent {
  std::string url;
  std::vector<std::string> tab_titles;
};

/*
 * Browser URLs and tab titles through UI Automation; best effort, like macOS
 * Accessibility. Calls time out, the walk is bounded, and it never enters a document.
 */
class BrowserAccessibility {
public:
  BrowserWindowContent Read(HWND window, uint64_t deadline_ns) {
    BrowserWindowContent content;
    if (!Ready()) return content;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation_->ElementFromHandle(window, &root)) || !root) return content;
    int budget = kAccessibilityMaxNodes;
    Visit(root.Get(), 0, deadline_ns, budget, content);
    return content;
  }

private:
  bool Ready() {
    if (walker_ || attempted_) return walker_ != nullptr;
    attempted_ = true;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation_))) &&
        FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation_)))) {
      return false;
    }
    ComPtr<IUIAutomation2> timeouts;
    if (SUCCEEDED(automation_.As(&timeouts))) {
      timeouts->put_ConnectionTimeout(kAccessibilityTimeoutMs);
      timeouts->put_TransactionTimeout(kAccessibilityTimeoutMs);
    }
    return SUCCEEDED(automation_->get_ControlViewWalker(&walker_)) && walker_;
  }

  void Visit(IUIAutomationElement *element, int depth, uint64_t deadline_ns,
             int &budget, BrowserWindowContent &content) {
    if (depth > kAccessibilityMaxDepth || budget-- <= 0 ||
        MonotonicTimeNs() > deadline_ns) {
      return;
    }
    CONTROLTYPEID type = 0;
    if (FAILED(element->get_CurrentControlType(&type))) return;
    switch (type) {
      case UIA_TabItemControlTypeId:
        if (std::string title = Name(element); !title.empty()) {
          content.tab_titles.push_back(std::move(title));
        }
        return;
      case UIA_EditControlTypeId:
      case UIA_DocumentControlTypeId:
        /* The omnibox, or the page itself in an app window without one. */
        if (content.url.empty()) {
          std::string value = Value(element);
          if (value.find('.') != std::string::npos || value.rfind("http", 0) == 0) {
            content.url = std::move(value);
          }
        }
        return;
    }
    ComPtr<IUIAutomationElement> child;
    walker_->GetFirstChildElement(element, &child);
    while (child) {
      Visit(child.Get(), depth + 1, deadline_ns, budget, content);
      ComPtr<IUIAutomationElement> next;
      if (FAILED(walker_->GetNextSiblingElement(child.Get(), &next))) break;
      child = std::move(next);
    }
  }

  static std::string Name(IUIAutomationElement *element) {
    BSTR name = nullptr;
    if (FAILED(element->get_CurrentName(&name)) || !name) return {};
    std::string result = Narrow(std::wstring(name, SysStringLen(name)));
    SysFreeString(name);
    return result;
  }

  static std::string Value(IUIAutomationElement *element) {
    VARIANT value;
    VariantInit(&value);
    std::string result;
    if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &value)) &&
        value.vt == VT_BSTR && value.bstrVal) {
      result = Narrow(std::wstring(value.bstrVal, SysStringLen(value.bstrVal)));
    }
    VariantClear(&value);
    return result;
  }

  ComPtr<IUIAutomation> automation_;
  ComPtr<IUIAutomationTreeWalker> walker_;
  bool attempted_ = false;
};

struct BrowserMeeting {
  const VisibleWindow *window = nullptr;
  mrec_platform platform = MREC_PLATFORM_UNKNOWN;
  std::string title;
  std::string url;
};

/* The front window showing a meeting in its URL, title or any tab; else the front window. */
template <typename Predicate>
BrowserMeeting FindBrowserMeeting(const std::vector<VisibleWindow> &windows,
                                  Predicate predicate,
                                  BrowserAccessibility &accessibility) {
  BrowserMeeting front;
  const uint64_t deadline_ns = MonotonicTimeNs() + kAccessibilityTraversalNs;
  int inspected = 0;
  for (const auto &window : windows) {
    if (!predicate(window)) continue;
    if (inspected++ == kMaxBrowserWindows) break;
    BrowserWindowContent content = accessibility.Read(window.handle, deadline_ns);
    if (!front.window) front = {&window, MREC_PLATFORM_UNKNOWN, window.title, content.url};

    std::string title = window.title;
    const auto title_platform = TitlePlatform(title);
    if (const auto platform = UrlPlatform(content.url);
        platform != MREC_PLATFORM_UNKNOWN) {
      return {&window, platform, std::move(title), std::move(content.url)};
    }
    if (title_platform != MREC_PLATFORM_UNKNOWN) {
      return {&window, title_platform, std::move(title), std::move(content.url)};
    }
    /* A background tab's URL is not exposed, only its title. */
    for (auto &tab : content.tab_titles) {
      if (const auto platform = TitlePlatform(tab); platform != MREC_PLATFORM_UNKNOWN) {
        return {&window, platform, std::move(tab), {}};
      }
    }
  }
  return front;
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
  ScopedComApartment apartment;
  const auto activity = CollectAudioActivity();
  const auto windows = CollectVisibleWindows();
  const auto processes = CollectProcesses(windows);
  const DWORD self = GetCurrentProcessId();
  DWORD shell_pid = 0;
  GetWindowThreadProcessId(GetShellWindow(), &shell_pid);

  /* Group by platform (native) or by the browser instance that owns the audio. */
  std::map<mrec_platform, Detected> native;
  std::map<mrec_platform, std::vector<DWORD>> native_owners;
  std::map<DWORD, Detected> browser;

  for (const auto &[pid, state] : activity) {
    if (pid == self) continue;
    if (!state.rendering && !state.capturing) continue;
    const auto process = processes.find(pid);
    if (process == processes.end()) continue;

    /* Audio often comes from a helper, such as new Teams' WebView2: use its owner app. */
    const auto *root = meeting_record::FindAudioAppRoot(processes, pid, shell_pid);
    std::string exe = Narrow(process->second.executable);
    const bool is_browser = IsBrowser(exe);
    mrec_platform platform = AppPlatform(exe);
    if (platform == MREC_PLATFORM_UNKNOWN && !is_browser && root) {
      exe = Narrow(root->executable);
      platform = AppPlatform(exe);
    }

    if (platform != MREC_PLATFORM_UNKNOWN) {
      Detected &entry = native[platform];
      entry.platform = platform;
      native_owners[platform].push_back(root ? root->pid : pid);
      entry.audio_pids.push_back(pid);
      entry.app_name = exe;
      entry.mic = entry.mic || state.capturing;
      entry.output = entry.output || state.rendering;
    } else if (is_browser) {
      Detected &entry = browser[root ? root->pid : pid];
      if (entry.pid == 0) entry.pid = root ? root->pid : pid;
      entry.audio_pids.push_back(pid);
      entry.app_name = exe;
      entry.mic = entry.mic || state.capturing;
      entry.output = entry.output || state.rendering;
    }
  }

  std::vector<Detected> results;

  for (auto &[platform, entry] : native) {
    /*
     * The pid is the capture target, so it must own the audio and stay put while
     * windows resize: the lowest owner with a window, else the lowest owner.
     */
    auto &owners = native_owners[platform];
    std::sort(owners.begin(), owners.end());
    const auto windowed = std::find_if(owners.begin(), owners.end(), [&processes](DWORD pid) {
      const auto process = processes.find(pid);
      return process != processes.end() && process->second.has_app_window;
    });
    entry.pid = windowed != owners.end() ? *windowed : owners.front();
    if (const auto *window = BestWindow(windows, [platform](const VisibleWindow &candidate) {
          return AppPlatform(candidate.executable) == platform;
        })) {
      entry.app_name = window->executable;
      entry.title = window->title;
    }
    entry.confidence =
        mrec_catalog_confidence(0, entry.output, entry.mic, MREC_PLATFORM_UNKNOWN);
    results.push_back(entry);
  }

  /* Only an instance's own windows describe it; another instance is another target. */
  BrowserAccessibility accessibility;
  for (auto &[owner, entry] : browser) {
    const DWORD instance = owner;
    BrowserMeeting meeting = FindBrowserMeeting(
        windows, [instance](const VisibleWindow &candidate) { return candidate.pid == instance; },
        accessibility);
    if (meeting.window) entry.app_name = meeting.window->executable;
    entry.title = std::move(meeting.title);
    entry.url = std::move(meeting.url);
    const mrec_platform matched = meeting.platform;
    entry.confidence = mrec_catalog_confidence(1, entry.output, entry.mic, matched);
    if (entry.confidence == 0) continue;
    entry.platform =
        matched != MREC_PLATFORM_UNKNOWN ? matched : MREC_PLATFORM_GENERIC_BROWSER;
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
  out->should_record = out->confidence >= MREC_CATALOG_RECORD_THRESHOLD ? 1 : 0;

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
        tracked_.clear();
        callback_ = nullptr;
        user_data_ = nullptr;
      }
    }
    if (stopping_from_callback) return MREC_OK;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    tracked_.clear();
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

  struct Tracked {
    Detected meeting;
    uint64_t first_seen_ns = 0;
    uint64_t last_seen_ns = 0;
    bool started = false;
  };

  void Rescan(uint64_t generation) {
    if (ShouldStop(generation)) return;
    const auto found = Scan();
    const uint64_t now = MonotonicTimeNs();

    mrec_meeting payload;
    for (const auto &entry : found) {
      if (ShouldStop(generation)) return;
      auto [it, inserted] = tracked_.try_emplace(entry.pid);
      Tracked &tracked = it->second;
      if (inserted) tracked.first_seen_ns = now;
      tracked.last_seen_ns = now;
      const bool changed = Changed(tracked.meeting, entry);
      tracked.meeting = entry;
      if (!tracked.started) {
        if (now - tracked.first_seen_ns < kStartDelayNs) continue;
        tracked.started = true;
        Fill(&payload, entry);
        callback_(&payload, MREC_MEETING_STARTED, user_data_);
      } else if (changed) {
        Fill(&payload, entry);
        callback_(&payload, MREC_MEETING_UPDATED, user_data_);
      }
    }
    for (auto it = tracked_.begin(); it != tracked_.end();) {
      if (ShouldStop(generation)) return;
      const Tracked &tracked = it->second;
      if (tracked.last_seen_ns == now ||
          (tracked.started && now - tracked.last_seen_ns < kEndDelayNs)) {
        ++it;
        continue;
      }
      if (tracked.started) {
        Fill(&payload, tracked.meeting);
        callback_(&payload, MREC_MEETING_ENDED, user_data_);
        if (ShouldStop(generation)) return;
      }
      it = tracked_.erase(it);
    }
  }

  static bool Changed(const Detected &a, const Detected &b) {
    return a.platform != b.platform || a.title != b.title || a.url != b.url ||
           a.mic != b.mic || a.output != b.output || a.audio_pids != b.audio_pids;
  }

  static constexpr uint64_t kStartDelayNs = MREC_CATALOG_START_DELAY_MS * 1000000ull;
  static constexpr uint64_t kEndDelayNs = MREC_CATALOG_END_DELAY_MS * 1000000ull;

  std::mutex mutex_;
  bool running_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> generation_{0};
  std::thread thread_;
  std::map<DWORD, Tracked> tracked_;
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

  /*
   * The visible owner's tree follows helpers created later; audio processes
   * outside it, such as Teams' call container, are added. Capture drops those
   * inside the tree. Refresh both, since the caller may have held the meeting
   * for a while; match the pid first, as two browsers can both host Meet.
   */
  const auto current = Scan();
  auto fresh = std::find_if(current.begin(), current.end(), [meeting](const Detected &item) {
    return item.pid == meeting->pid;
  });
  if (fresh == current.end()) {
    fresh = std::find_if(current.begin(), current.end(), [meeting](const Detected &item) {
      return item.platform == meeting->platform;
    });
  }
  std::vector<uint32_t> targets;
  if (fresh != current.end()) {
    targets.push_back(fresh->pid);
    targets.insert(targets.end(), fresh->audio_pids.begin(), fresh->audio_pids.end());
  } else {
    targets.push_back(meeting->pid);
    targets.insert(targets.end(), meeting->audio_pids,
                   meeting->audio_pids +
                       (std::min)(meeting->audio_pid_count, size_t{MREC_MAX_AUDIO_PIDS}));
  }
  targets.erase(std::remove(targets.begin(), targets.end(), 0u), targets.end());
  if (targets.empty()) return MREC_ERR_NO_PROCESSES;

  return static_cast<mrec_status>(mrec_start_raw(targets.data(), targets.size(), 0, 1, 0,
                                                 callback, user_data));
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
  return MREC_PERM_GRANTED;
}

mrec_status mrec_request_accessibility_permission(void) { return MREC_OK; }

} // extern "C"
