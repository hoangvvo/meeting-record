#include "../windows/MeetingCatalog.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  using meeting_record::IsBrowser;
  using meeting_record::MeetTitle;
  using meeting_record::NativePlatformFor;
  using meeting_record::PlatformForText;
  using meeting_record::PlatformForTitle;

  // Native clients match on their lowercased executable, including media hosts.
  assert(NativePlatformFor("zoom.exe") == MREC_PLATFORM_ZOOM);
  assert(NativePlatformFor("cpthost.exe") == MREC_PLATFORM_ZOOM);
  assert(NativePlatformFor("ms-teams.exe") == MREC_PLATFORM_TEAMS);
  assert(NativePlatformFor("ms-teams_modulehost.exe") == MREC_PLATFORM_TEAMS);
  assert(NativePlatformFor("discord.exe") == MREC_PLATFORM_DISCORD);
  assert(NativePlatformFor("") == MREC_PLATFORM_UNKNOWN);
  // A browser must not count as a native client, or every video tab is a meeting.
  assert(NativePlatformFor("msedge.exe") == MREC_PLATFORM_UNKNOWN);
  assert(IsBrowser("msedge.exe"));
  assert(IsBrowser("chrome.exe"));
  assert(!IsBrowser("zoom.exe"));
  assert(!IsBrowser("msedgewebview2.exe"));

  // Address-bar values may omit the scheme, and matching ignores case.
  assert(PlatformForText("https://meet.google.com/abc-defg-hij") == MREC_PLATFORM_MEET);
  assert(PlatformForText("MEET.GOOGLE.COM/abc-defg-hij") == MREC_PLATFORM_MEET);
  assert(PlatformForText("https://teams.microsoft.com/l/meetup-join/1") == MREC_PLATFORM_TEAMS);
  assert(PlatformForText("https://us02web.zoom.us/j/123456789") == MREC_PLATFORM_ZOOM);
  assert(PlatformForText("https://webex.com/meet/someone") == MREC_PLATFORM_WEBEX);
  assert(PlatformForText("https://discord.com/channels/1/2") == MREC_PLATFORM_DISCORD);
  assert(PlatformForText("https://app.gather.town/app/office") == MREC_PLATFORM_GENERIC_BROWSER);
  assert(PlatformForText("https://www.youtube.com/watch?v=1") == MREC_PLATFORM_UNKNOWN);
  assert(PlatformForText("") == MREC_PLATFORM_UNKNOWN);

  // Meet titles carry the meeting code; browsers append their own suffixes.
  assert(MeetTitle("Meet - abc-defg-hij") == "Meet - abc-defg-hij");
  assert(MeetTitle("Meet - abc-defg-hij - Part of tab group Work - Audio playing") ==
         "Meet - abc-defg-hij");
  assert(MeetTitle("Meet - abc-defg-hij and 3 more pages - Profile 2 - Microsoft Edge") ==
         "Meet - abc-defg-hij");
  assert(MeetTitle("Meet - abc-defg-hijk").empty());
  assert(MeetTitle("Meet - ABC-defg-hij").empty());
  assert(MeetTitle("Meet - the team").empty());
  assert(MeetTitle("Google Meet").empty());
  assert(MeetTitle("Meet").empty());

  std::string title = "Meet - abc-defg-hij - Memory usage - 403 MB";
  assert(PlatformForTitle(title) == MREC_PLATFORM_MEET);
  assert(title == "Meet - abc-defg-hij");
  // Other titles fall back to URL fragments and stay unchanged.
  title = "Daily standup | meet.jit.si";
  assert(PlatformForTitle(title) == MREC_PLATFORM_GENERIC_BROWSER);
  assert(title == "Daily standup | meet.jit.si");
  title = "Inbox - Outlook";
  assert(PlatformForTitle(title) == MREC_PLATFORM_UNKNOWN);
  std::cout << "meeting catalog tests passed\n";
}
