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
 * after capture began. */
template <typename Processes>
std::vector<uint32_t> UncapturedRoots(const Processes &processes,
                                      const std::vector<uint32_t> &roots,
                                      const std::vector<uint32_t> &captured) {
  std::vector<uint32_t> found;
  for (uint32_t root : roots) {
    if (!InTrees(processes, root, captured)) found.push_back(root);
  }
  return found;
}

} // namespace meeting_record
