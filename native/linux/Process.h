#pragma once

#include "meeting-record.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <time.h>
#include <unistd.h>

namespace meeting_record::linux_backend {
inline uint64_t NowNs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
}

struct Process {
  uint32_t pid = 0, parent = 0;
  uint64_t birth = 0;
  std::string executable, name;
};
inline Process ReadProcess(uint32_t pid) {
  Process result;
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(input, line))
    return result;
  const auto end = line.rfind(')');
  if (end == std::string::npos)
    return result;
  std::istringstream fields(line.substr(end + 2));
  std::string value;
  // Fields 3 through 22; comm can contain spaces and parentheses.
  for (int field = 3; field <= 22; ++field) {
    if (!(fields >> value))
      return {};
    if (field == 4)
      result.parent = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    if (field == 22)
      result.birth = std::strtoull(value.c_str(), nullptr, 10);
  }
  std::error_code error;
  auto path = std::filesystem::read_symlink(
      "/proc/" + std::to_string(pid) + "/exe", error);
  if (error)
    return {};
  result.pid = pid;
  result.executable = path.string();
  result.name = path.filename().string();
  return result;
}
inline bool Descendant(uint32_t pid, uint32_t ancestor) {
  for (unsigned depth = 0; pid > 1 && depth < 128; ++depth) {
    if (pid == ancestor)
      return true;
    const auto process = ReadProcess(pid);
    if (!process.pid || process.parent == pid)
      break;
    pid = process.parent;
  }
  return false;
}
inline Process AppRoot(uint32_t pid) {
  auto root = ReadProcess(pid);
  auto current = root;
  // Same executable helpers belong to the same browser/app instance. Do not
  // merge separate instances merely because their executable names match.
  for (unsigned depth = 0; current.parent > 1 && depth < 128; ++depth) {
    const auto parent = ReadProcess(current.parent);
    if (!parent.pid || parent.executable != root.executable)
      break;
    root = current = parent;
  }
  return root;
}
template <size_t N> inline void Copy(char (&out)[N], const std::string &text) {
  std::snprintf(out, N, "%s", text.c_str());
}
} // namespace meeting_record::linux_backend
