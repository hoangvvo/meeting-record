#include "../common/windows-capture-policy.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

int main() {
  using meeting_record::WindowsCaptureCompatibilityApp;
  assert(std::strcmp(WindowsCaptureCompatibilityApp("whatsapp.root.exe"), "net.whatsapp.WhatsApp") == 0);
  assert(std::strcmp(WindowsCaptureCompatibilityApp("8x8 work.exe"), "com.electron.8x8---virtual-office") == 0);
  assert(std::strcmp(WindowsCaptureCompatibilityApp("ms-teams_modulehost.exe"), "com.microsoft.teams2") == 0);
  assert(std::strcmp(WindowsCaptureCompatibilityApp("MS-TEAMS.EXE"), "com.microsoft.teams2") == 0);
  for (const char *unmatched : {"", "chrome.exe", "msedgewebview2.exe", "zoom.exe",
                               "not-ms-teams.exe", "ms-teams.exe.backup", "8x8.exe"}) {
    assert(WindowsCaptureCompatibilityApp(unmatched) == nullptr);
  }
  assert(WindowsCaptureCompatibilityApp(nullptr) == nullptr);
}
