#pragma once
#include "meeting-record-detect.h"
#include <map>
#include <string>
#include <vector>

namespace meeting_record::linux_backend {
struct WindowEvidence {
  std::string title, url;
  int32_t platform = MREC_PLATFORM_UNKNOWN;
  int32_t call_state = 0;
};
struct AppEvidence {
  uint32_t pid = 0;
  std::vector<WindowEvidence> windows;
};
std::map<uint32_t, AppEvidence> ReadAccessibility();
bool AccessibilityAvailable();
} // namespace meeting_record::linux_backend
