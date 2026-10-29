#include "../linux/Accessibility.h"
#include "../linux/Process.h"
#include "meeting-record-detect.h"
#include <cstdio>

int main() {
  std::printf("audio permission=%d accessibility=%d\n",
              mrec_audio_permission_status(),
              mrec_accessibility_permission_status());
  for (const auto &[pid, app] :
       meeting_record::linux_backend::ReadAccessibility()) {
    std::printf(
        "accessible app pid=%u executable=%s windows=%zu\n", pid,
        meeting_record::linux_backend::ReadProcess(pid).executable.c_str(),
        app.windows.size());
    for (const auto &window : app.windows)
      std::printf("  platform=%d state=%d title=%s url=%s\n", window.platform,
                  window.call_state, window.title.c_str(), window.url.c_str());
  }
  mrec_meeting meetings[32]{};
  size_t count = 0;
  const auto status = mrec_scan(meetings, 32, &count);
  std::printf("scan status=%d count=%zu\n", status, count);
  for (size_t i = 0; i < count; ++i)
    std::printf(
        "  pid=%u platform=%d confidence=%d should_record=%d title=%s\n",
        meetings[i].pid, meetings[i].platform, meetings[i].confidence,
        meetings[i].should_record, meetings[i].title);
}
