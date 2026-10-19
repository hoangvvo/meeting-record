#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <map>
#include <string>

namespace meeting_record {

struct AudioAppProcess {
  uint32_t pid;
  uint32_t parent_pid;
  std::wstring executable;
  bool has_app_window = false;
};

inline bool SameExecutable(const std::wstring &left, const std::wstring &right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](wchar_t a, wchar_t b) {
                      return std::towlower(a) == std::towlower(b);
                    });
}

inline const AudioAppProcess *FindAudioAppRoot(
    const std::map<uint32_t, AudioAppProcess> &processes, uint32_t pid,
    uint32_t shell_pid) {
  auto current = processes.find(pid);
  if (current == processes.end() || pid == shell_pid) return nullptr;
  const AudioAppProcess *root = &current->second;
  bool same_executable_chain = true;

  // A visible window identifies the owning app even when its helper has a
  // different executable. Hidden/tray apps retain their same-executable root.
  for (size_t remaining = processes.size(); remaining > 0; remaining--) {
    if (current->second.has_app_window) return &current->second;
    const auto parent = processes.find(current->second.parent_pid);
    if (parent == processes.end() || parent == current ||
        parent->second.pid == shell_pid) break;
    same_executable_chain = same_executable_chain &&
        SameExecutable(current->second.executable, parent->second.executable);
    if (same_executable_chain) root = &parent->second;
    current = parent;
  }
  return root;
}

} // namespace meeting_record
