#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace meeting_record {

/* Whether `pid` is one of `roots` or descends from one. `processes` maps a pid to an
 * entry with its `parent`; a root that exited still owns the children it left. */
template <typename Processes>
bool InTrees(const Processes &processes, uint32_t pid, const std::vector<uint32_t> &roots) {
  for (size_t remaining = processes.size() + 1; remaining > 0; remaining--) {
    if (std::find(roots.begin(), roots.end(), pid) != roots.end()) return true;
    const auto process = processes.find(pid);
    if (process == processes.end() || process->second.parent == pid) return false;
    pid = process->second.parent;
  }
  return false;
}

/* A captured process: its pid, and its creation time, which tells it apart from a
 * later process that reuses the pid. 0 when the time could not be read. */
struct Root {
  uint32_t pid;
  uint64_t created;
};

/*
 * Whether `pid` is one of `roots` or descends from one. Unlike InTrees, a pid
 * reused by a later process is not the process it replaced: a parent link holds
 * only if that parent existed when the child was created. `created(pid)` returns
 * a process's creation time, or 0 when unknown, which falls back to matching pids.
 */
template <typename Processes, typename Created>
bool InRootTrees(const Processes &processes, uint32_t pid, const std::vector<Root> &roots,
                 Created &&created) {
  const auto matches = [](uint64_t time, uint64_t root_time) {
    return time == 0 || root_time == 0 || time == root_time;
  };
  uint64_t time = created(pid);
  for (size_t remaining = processes.size() + 1; remaining > 0; remaining--) {
    for (const Root &root : roots) {
      if (root.pid == pid && matches(time, root.created)) return true;
    }
    const auto process = processes.find(pid);
    if (process == processes.end() || process->second.parent == pid) return false;
    const uint32_t parent = process->second.parent;
    const bool present = processes.find(parent) != processes.end();
    const uint64_t parent_time = present ? created(parent) : 0;
    if (!present || (time != 0 && parent_time > time)) {
      // The real parent exited. It was a root if one with that pid predates the child.
      for (const Root &root : roots) {
        if (root.pid == parent && (time == 0 || root.created <= time)) return true;
      }
      return false;
    }
    pid = parent;
    time = parent_time;
  }
  return false;
}

/* `pids` plus every process `sibling` accepts, keeping only outermost trees: a
 * second client would capture a nested tree's audio twice. */
template <typename Processes, typename Sibling>
std::vector<uint32_t> SelectCaptureRoots(const Processes &processes, std::vector<uint32_t> pids,
                                         Sibling &&sibling) {
  for (const auto &[pid, process] : processes) {
    if (sibling(pid, process)) pids.push_back(pid);
  }
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());

  std::vector<uint32_t> roots;
  for (uint32_t pid : pids) {
    const auto process = processes.find(pid);
    const bool nested = process != processes.end() && process->second.parent != pid &&
                        InTrees(processes, process->second.parent, pids);
    if (!nested) roots.push_back(pid);
  }
  return roots;
}

/* Roots outside every captured tree, such as an app's helper launched by the system
 * after capture began, or a new process that reuses a captured process's pid. */
template <typename Processes, typename Created>
std::vector<uint32_t> UncapturedRoots(const Processes &processes,
                                      const std::vector<uint32_t> &roots,
                                      const std::vector<Root> &captured, Created &&created) {
  std::vector<uint32_t> found;
  for (uint32_t root : roots) {
    if (!InRootTrees(processes, root, captured, created)) found.push_back(root);
  }
  return found;
}

} // namespace meeting_record
