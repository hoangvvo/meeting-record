#pragma once
#include "meeting-catalog.h"
#include "meeting-record-detect.h"

namespace meeting_record {
struct CaptureCompatibilityApp {
  int32_t platform;
  const char *app_id;
};

// Apps whose Windows audio may require endpoint capture. App identity selects
// a compatibility policy; it does not identify a stream's audio category.
inline constexpr CaptureCompatibilityApp kWindowsCaptureCompatibilityApps[] = {
    {MREC_PLATFORM_TEAMS, "com.microsoft.teams2"},
    {MREC_PLATFORM_WHATSAPP, "net.whatsapp.WhatsApp"},
    {MREC_PLATFORM_8X8, "com.electron.8x8---virtual-office"},
};

inline const char *WindowsCaptureCompatibilityApp(const char *executable) {
  // Reuse the shared executable aliases; capture policy does not own another map.
  const int32_t platform = mrec_catalog_executable_platform(executable);
  for (const auto &app : kWindowsCaptureCompatibilityApps) {
    if (platform == app.platform) return app.app_id;
  }
  return nullptr;
}
} // namespace meeting_record
