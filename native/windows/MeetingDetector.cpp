/* Meeting detection from audio sessions, windows, and browser accessibility. */
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

/* Bound synchronous UI Automation calls. */
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
  /* Reserve space for the terminating NUL before trimming it. */
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
  mrec_platform call_platform = MREC_PLATFORM_UNKNOWN;
  uint32_t call_signals = 0;
};

/* Inspect the browser shell first; probe controls only within a known service. */
class BrowserAccessibility {
public:
  BrowserWindowContent Read(HWND window, uint64_t deadline_ns, const std::string &title,
                            mrec_platform native_platform = MREC_PLATFORM_UNKNOWN) {
    BrowserWindowContent content;
    if (MonotonicTimeNs() >= deadline_ns || !Ready() || MonotonicTimeNs() >= deadline_ns) return content;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation_->ElementFromHandle(window, &root)) || !root) return content;
    if (native_platform != MREC_PLATFORM_UNKNOWN) {
      content.call_platform = native_platform;
      int budget = 1200;
      Probe(root.Get(), 0, deadline_ns, budget, content, false, false);
      return content;
    }
    int budget = kAccessibilityMaxNodes;
    std::vector<ComPtr<IUIAutomationElement>> documents;
    Visit(root.Get(), 0, deadline_ns, budget, content, documents);
    content.call_platform = static_cast<mrec_platform>(mrec_catalog_context_platform(content.url.c_str(), title.c_str()));
    if (content.call_platform == MREC_PLATFORM_UNKNOWN) return content;
    if (documents.empty()) documents = {root};
    int call_budget = 1200;
    for (const auto &document : documents) {
      Probe(document.Get(), 0, deadline_ns, call_budget, content, false, true);
      if (call_budget <= 0 || MonotonicTimeNs() >= deadline_ns) break;
    }
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
             int &budget, BrowserWindowContent &content,
             std::vector<ComPtr<IUIAutomationElement>> &documents) {
    if (depth > kAccessibilityMaxDepth || budget-- <= 0 ||
        MonotonicTimeNs() > deadline_ns) {
      return;
    }
    BOOL offscreen = FALSE;
    if (SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && offscreen) return;
    if (MonotonicTimeNs() >= deadline_ns) return;
    CONTROLTYPEID type = 0;
    if (FAILED(element->get_CurrentControlType(&type)) || MonotonicTimeNs() >= deadline_ns) return;
    switch (type) {
      case UIA_TabItemControlTypeId:
        if (std::string title = Name(element); !title.empty()) {
          content.tab_titles.push_back(std::move(title));
        }
        return;
      case UIA_DocumentControlTypeId:
        documents.emplace_back(element);
        [[fallthrough]];
      case UIA_EditControlTypeId:
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
    while (child && budget > 0 && MonotonicTimeNs() < deadline_ns) {
      Visit(child.Get(), depth + 1, deadline_ns, budget, content, documents);
      if (budget <= 0 || MonotonicTimeNs() >= deadline_ns) break;
      ComPtr<IUIAutomationElement> next;
      if (FAILED(walker_->GetNextSiblingElement(child.Get(), &next))) break;
      child = std::move(next);
    }
  }

  void Probe(IUIAutomationElement *element, int depth, uint64_t deadline_ns,
             int &budget, BrowserWindowContent &content, bool status_context, bool scope_documents) {
    if (depth > kAccessibilityMaxDepth || budget <= 0 || MonotonicTimeNs() >= deadline_ns) return;
    budget--;
    BOOL offscreen = FALSE;
    if (SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && offscreen) return;
    if (MonotonicTimeNs() >= deadline_ns) return;
    CONTROLTYPEID type = 0;
    if (FAILED(element->get_CurrentControlType(&type)) || MonotonicTimeNs() >= deadline_ns) return;
    if (type == UIA_EditControlTypeId) return;
    if (scope_documents && type == UIA_DocumentControlTypeId) {
      const std::string url = Value(element);
      if ((url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) &&
          mrec_catalog_context_platform(url.c_str(), "") != content.call_platform) return;
    }
    std::string aria_role;
    if (type == UIA_GroupControlTypeId || type == UIA_TextControlTypeId) {
      aria_role = StringProperty(element, UIA_AriaRolePropertyId);
    }
    const bool status = status_context || type == UIA_StatusBarControlTypeId ||
        aria_role == "status" || aria_role == "alert" || aria_role == "heading";
    if (MonotonicTimeNs() >= deadline_ns) return;
    int32_t role = 0;
    if (type == UIA_ButtonControlTypeId || type == UIA_SplitButtonControlTypeId) role = MREC_CONTROL_BUTTON;
    else if (type == UIA_MenuItemControlTypeId) role = MREC_CONTROL_MENU_ITEM;
    else if (status && (type == UIA_TextControlTypeId || type == UIA_StatusBarControlTypeId)) role = MREC_CONTROL_STATUS;
    else if (content.call_platform == MREC_PLATFORM_DISCORD &&
             (type == UIA_TextControlTypeId || type == UIA_HyperlinkControlTypeId)) role = MREC_CONTROL_TEXT;
    if (role) {
      const std::string identifier = StringProperty(element, UIA_AutomationIdPropertyId);
      content.call_signals |= mrec_catalog_call_control(content.call_platform, role, identifier.c_str(), "");
      if (MonotonicTimeNs() >= deadline_ns) return;
      const std::string label = Name(element);
      content.call_signals |= mrec_catalog_call_control(content.call_platform, role, "", label.c_str());
    }
    if (MonotonicTimeNs() >= deadline_ns) return;
    ComPtr<IUIAutomationElement> child;
    walker_->GetLastChildElement(element, &child);
    while (child && budget > 0 && MonotonicTimeNs() < deadline_ns) {
      Probe(child.Get(), depth + 1, deadline_ns, budget, content, status, scope_documents);
      if (budget <= 0 || MonotonicTimeNs() >= deadline_ns) break;
      ComPtr<IUIAutomationElement> previous;
      if (FAILED(walker_->GetPreviousSiblingElement(child.Get(), &previous))) break;
      child = std::move(previous);
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
    return StringProperty(element, UIA_ValueValuePropertyId);
  }

  static std::string StringProperty(IUIAutomationElement *element, PROPERTYID property) {
    VARIANT value;
    VariantInit(&value);
    std::string result;
    if (SUCCEEDED(element->GetCurrentPropertyValue(property, &value)) &&
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
  int32_t call_state = MREC_CALL_UNKNOWN;
};

/* The front window showing a meeting in its URL, title or any tab; else the front window. */
template <typename Predicate>
BrowserMeeting FindBrowserMeeting(const std::vector<VisibleWindow> &windows,
                                  Predicate predicate,
                                  BrowserAccessibility &accessibility) {
  BrowserMeeting front;
  BrowserMeeting fallback;
  const uint64_t deadline_ns = MonotonicTimeNs() + kAccessibilityTraversalNs;
  int inspected = 0;
  for (const auto &window : windows) {
    if (!predicate(window)) continue;
    if (inspected++ == kMaxBrowserWindows) break;
    BrowserWindowContent content = accessibility.Read(window.handle, deadline_ns, window.title);
    if (!front.window) front = {&window, MREC_PLATFORM_UNKNOWN, window.title, content.url};
    const int32_t state = mrec_catalog_call_state(content.call_signals);
    if (state == MREC_CALL_JOINED) {
      return {&window, content.call_platform, window.title, std::move(content.url), state};
    }
    // Pre-join content must not borrow a stale meeting title from another tab.
    if (state == MREC_CALL_NOT_JOINED) continue;

    std::vector<const char *> tabs;
    tabs.reserve(content.tab_titles.size());
    for (const auto &tab : content.tab_titles) tabs.push_back(tab.c_str());
    const auto match = mrec_catalog_browser_match(
        content.url.c_str(), window.title.c_str(), tabs.data(), tabs.size());
    if (match.platform == MREC_PLATFORM_UNKNOWN) continue;
    const std::string &title = match.tab_index >= 0
                                   ? content.tab_titles[static_cast<size_t>(match.tab_index)]
                                   : window.title;
    if (!fallback.window) fallback = {&window, static_cast<mrec_platform>(match.platform),
            title.substr(0, match.title_length),
            match.tab_index >= 0 ? std::string{} : std::move(content.url)};
  }
  return fallback.window ? fallback : front;
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

BrowserMeeting FindNativeMeeting(const std::vector<VisibleWindow> &windows, DWORD owner,
                                 mrec_platform platform, BrowserAccessibility &accessibility) {
  BrowserMeeting result;
  if (const auto *window = BestWindow(windows, [owner](const VisibleWindow &w) { return w.pid == owner; })) {
    result = {window, platform, window->title, {}};
  }
  const uint64_t deadline = MonotonicTimeNs() + kAccessibilityTraversalNs;
  int inspected = 0;
  for (const auto &window : windows) {
    if (window.pid != owner) continue;
    if (inspected++ == kMaxBrowserWindows || MonotonicTimeNs() >= deadline) break;
    const auto content = accessibility.Read(window.handle, deadline, window.title, platform);
    const int32_t state = mrec_catalog_call_state(content.call_signals);
    if (state == MREC_CALL_JOINED) return {&window, platform, window.title, {}, state};
    if (state == MREC_CALL_NOT_JOINED) result = {&window, platform, window.title, {}, state};
  }
  return result;
}

std::vector<Detected> Scan() {
  ScopedComApartment apartment;
  const auto activity = CollectAudioActivity();
  const auto windows = CollectVisibleWindows();
  const auto processes = CollectProcesses(windows);
  const DWORD self = GetCurrentProcessId();
  DWORD shell_pid = 0;
  GetWindowThreadProcessId(GetShellWindow(), &shell_pid);

  /* Only combine activity from helpers belonging to the same app instance. */
  std::map<DWORD, Detected> native;
  std::map<DWORD, Detected> browser;

  for (const auto &[pid, state] : activity) {
    if (pid == self) continue;
    if (!state.rendering && !state.capturing) continue;
    const auto process = processes.find(pid);
    if (process == processes.end()) continue;

    /* Attribute helper audio to its owning app. */
    const auto *root = meeting_record::FindAudioAppRoot(processes, pid, shell_pid);
    if (root && root->pid == self) continue;
    std::string exe = Narrow(process->second.executable);
    const bool is_browser = IsBrowser(exe);
    mrec_platform platform = AppPlatform(exe);
    if (platform == MREC_PLATFORM_UNKNOWN && !is_browser && root) {
      exe = Narrow(root->executable);
      platform = AppPlatform(exe);
    }

    if (platform != MREC_PLATFORM_UNKNOWN) {
      const DWORD owner = root ? root->pid : pid;
      Detected &entry = native[owner];
      entry.pid = owner;
      entry.platform = platform;
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

  // Confirmed UI can identify a silent call before either audio stream opens.
  for (const auto &window : windows) {
    const mrec_platform platform = AppPlatform(window.executable);
    if (platform != MREC_PLATFORM_UNKNOWN) {
      auto &entry = native[window.pid];
      entry.pid = window.pid;
      entry.platform = platform;
      entry.app_name = window.executable;
    } else if (IsBrowser(window.executable)) {
      auto &entry = browser[window.pid];
      entry.pid = window.pid;
      entry.app_name = window.executable;
    }
  }

  std::vector<Detected> results;
  BrowserAccessibility accessibility;

  for (auto &[owner, entry] : native) {
    const auto meeting = FindNativeMeeting(windows, owner, entry.platform, accessibility);
    if (meeting.window) {
      entry.app_name = meeting.window->executable;
      entry.title = meeting.title;
    }
    entry.confidence =
        mrec_catalog_call_confidence(0, entry.output, entry.mic, entry.platform, meeting.call_state);
    if (entry.confidence == 0) continue;
    if (entry.audio_pids.empty()) entry.audio_pids.push_back(owner);
    results.push_back(entry);
  }

  /* Only an instance's own windows describe it; another instance is another target. */
  for (auto &[owner, entry] : browser) {
    const DWORD instance = owner;
    BrowserMeeting meeting = FindBrowserMeeting(
        windows, [instance](const VisibleWindow &candidate) { return candidate.pid == instance; },
        accessibility);
    if (meeting.window) entry.app_name = meeting.window->executable;
    entry.title = std::move(meeting.title);
    entry.url = std::move(meeting.url);
    const mrec_platform matched = meeting.platform;
    entry.confidence = mrec_catalog_call_confidence(1, entry.output, entry.mic, matched, meeting.call_state);
    if (entry.confidence == 0) continue;
    if (entry.audio_pids.empty()) entry.audio_pids.push_back(owner);
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
  /* Poll because session notifications do not report inactive-to-active changes. */
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
           a.mic != b.mic || a.output != b.output || a.audio_pids != b.audio_pids ||
           a.confidence != b.confidence;
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

  /* Refresh helpers for this owner PID without selecting another meeting. */
  const auto current = Scan();
  auto fresh = std::find_if(current.begin(), current.end(), [meeting](const Detected &item) {
    return item.pid == meeting->pid;
  });
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
