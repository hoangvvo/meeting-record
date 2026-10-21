#include "meeting-catalog.h"
#include "meeting-record-detect.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The page title `title` narrows to. */
static int PageTitleIs(const char *title, const char *expected) {
  size_t length = 0;
  mrec_catalog_title_platform(title, &length);
  return length == strlen(expected) && strncmp(title, expected, length) == 0;
}

int main(void) {
  /* macOS: bundle-id prefixes reach the helpers that carry the audio. */
  assert(mrec_catalog_bundle_platform("us.zoom.xos") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("us.zoom.xos.cpthost") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("com.microsoft.teams2") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_bundle_platform("com.microsoft.teams") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_bundle_platform("com.tinyspeck.slackmacgap.helper") ==
         MREC_PLATFORM_SLACK);
  assert(mrec_catalog_bundle_platform("US.ZOOM.XOS") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("com.apple.Music") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_bundle_platform("") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_bundle_platform(NULL) == MREC_PLATFORM_UNKNOWN);
  /* A browser must not count as a native client, or every video tab is a meeting. */
  assert(mrec_catalog_bundle_platform("com.google.Chrome") == MREC_PLATFORM_UNKNOWN);
  assert(strcmp(mrec_catalog_bundle_browser("com.google.Chrome"), "com.google.chrome") == 0);
  assert(strcmp(mrec_catalog_bundle_browser("com.google.Chrome.helper"),
                "com.google.chrome") == 0);
  assert(strcmp(mrec_catalog_bundle_browser("company.thebrowser.Browser"),
                "company.thebrowser.browser") == 0);
  assert(strcmp(mrec_catalog_bundle_browser("org.chromium.Chromium"), "org.chromium.chromium") == 0);
  assert(mrec_catalog_bundle_browser("us.zoom.xos") == NULL);

  /* Windows: executable names, including media hosts, in any case. */
  assert(mrec_catalog_executable_platform("zoom.exe") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_executable_platform("Zoom.exe") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_executable_platform("cpthost.exe") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_executable_platform("ms-teams.exe") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_executable_platform("ms-teams_modulehost.exe") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_executable_platform("discord.exe") == MREC_PLATFORM_DISCORD);
  assert(mrec_catalog_executable_platform("CiscoCollabHost.exe") == MREC_PLATFORM_WEBEX);
  assert(mrec_catalog_executable_platform("zoom.exe.bak") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_executable_platform("") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_executable_platform("msedge.exe") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_executable_browser("msedge.exe") != NULL);
  assert(mrec_catalog_executable_browser("CHROME.EXE") != NULL);
  assert(mrec_catalog_executable_browser("zen.exe") != NULL);
  assert(mrec_catalog_executable_browser("zoom.exe") == NULL);
  assert(mrec_catalog_executable_browser("msedgewebview2.exe") == NULL);

  /* URLs may omit the scheme, and matching ignores case. */
  assert(mrec_catalog_url_platform("https://meet.google.com/abc-defg-hij") == MREC_PLATFORM_MEET);
  assert(mrec_catalog_url_platform("MEET.GOOGLE.COM/abc-defg-hij") == MREC_PLATFORM_MEET);
  assert(mrec_catalog_url_platform("https://meet.google.com/abc-defg-hij?authuser=1") ==
         MREC_PLATFORM_MEET);
  /* Meet needs a meeting code: its home page and lookups are not calls. */
  assert(mrec_catalog_url_platform("https://meet.google.com/") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://meet.google.com/landing") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://meet.google.com/abc-defg-hijk") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://teams.microsoft.com/l/meetup-join/1") ==
         MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://teams.cloud.microsoft/v2/") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://dod.teams.microsoft.us/") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://us02web.zoom.us/j/123456789") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_url_platform("https://us06web.zoom.us/w/123?tk=1") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_url_platform("https://agency.zoomgov.com/j/123") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_url_platform("https://webex.com/meet/someone") == MREC_PLATFORM_WEBEX);
  assert(mrec_catalog_url_platform("https://app.slack.com/huddle/T01/C02") == MREC_PLATFORM_SLACK);
  assert(mrec_catalog_url_platform("https://discord.com/channels/1/2") == MREC_PLATFORM_DISCORD);
  assert(mrec_catalog_url_platform("https://app.gather.town/app/office") ==
         MREC_PLATFORM_GENERIC_BROWSER);
  assert(mrec_catalog_url_platform("https://zoom.us/pricing") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://www.youtube.com/watch?v=1") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform(NULL) == MREC_PLATFORM_UNKNOWN);

  /* Meet titles carry the meeting code; browsers append their own suffixes. */
  assert(mrec_catalog_title_platform("Meet - abc-defg-hij", NULL) == MREC_PLATFORM_MEET);
  assert(PageTitleIs("Meet - abc-defg-hij", "Meet - abc-defg-hij"));
  assert(PageTitleIs("Meet - abc-defg-hij - Part of tab group Work - Audio playing",
                     "Meet - abc-defg-hij"));
  assert(PageTitleIs("Meet - abc-defg-hij and 3 more pages - Profile 2 - Microsoft Edge",
                     "Meet - abc-defg-hij"));
  assert(PageTitleIs("Meet \xE2\x80\x93 abc-defg-hij - Google Chrome",
                     "Meet \xE2\x80\x93 abc-defg-hij"));
  assert(PageTitleIs("Meet \xE2\x80\x94" "abc-defg-hij", "Meet \xE2\x80\x94" "abc-defg-hij"));
  assert(mrec_catalog_title_platform("Meet -abc-defg-hij", NULL) == MREC_PLATFORM_MEET);
  assert(mrec_catalog_title_platform("Meet- abc-defg-hij", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_title_platform("Meeting - abc-defg-hij", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_title_platform("Meet - abc-defg-hijk", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_title_platform("Meet - ABC-defg-hij", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_title_platform("Meet - the team", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_title_platform("Google Meet", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_title_platform("Meet", NULL) == MREC_PLATFORM_UNKNOWN);
  /* Other titles fall back to URL fragments and stay whole. */
  assert(mrec_catalog_title_platform("Daily standup | meet.jit.si", NULL) ==
         MREC_PLATFORM_GENERIC_BROWSER);
  assert(PageTitleIs("Daily standup | meet.jit.si", "Daily standup | meet.jit.si"));
  assert(mrec_catalog_title_platform("Sync | meet.google.com/abc-defg-hij - Google Chrome", NULL) ==
         MREC_PLATFORM_MEET);
  assert(mrec_catalog_title_platform("Inbox - Outlook", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(PageTitleIs("", ""));

  /* Scores: an app needs audio; a browser also needs a recognised meeting or the mic. */
  assert(mrec_catalog_confidence(0, 0, 0, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(0, 1, 0, MREC_PLATFORM_UNKNOWN) == 70);
  assert(mrec_catalog_confidence(0, 0, 1, MREC_PLATFORM_UNKNOWN) == 65);
  assert(mrec_catalog_confidence(0, 1, 1, MREC_PLATFORM_UNKNOWN) == 95);
  assert(mrec_catalog_confidence(0, 0, 1, MREC_PLATFORM_ZOOM) == 65);
  assert(mrec_catalog_confidence(1, 1, 0, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(1, 0, 0, MREC_PLATFORM_MEET) == 0);
  assert(mrec_catalog_confidence(1, 1, 0, MREC_PLATFORM_MEET) == 75);
  assert(mrec_catalog_confidence(1, 0, 1, MREC_PLATFORM_UNKNOWN) == 65);
  assert(mrec_catalog_confidence(1, 0, 1, MREC_PLATFORM_MEET) == MREC_CATALOG_RECORD_THRESHOLD);
  assert(mrec_catalog_confidence(1, 1, 1, MREC_PLATFORM_MEET) == 100);

  /* Identifiers are stable. */
  assert(strcmp(mrec_platform_id(MREC_PLATFORM_MEET), "meet") == 0);
  assert(strcmp(mrec_platform_id(MREC_PLATFORM_GENERIC_BROWSER), "browser") == 0);
  assert(strcmp(mrec_platform_id((mrec_platform)99), "unknown") == 0);

  printf("meeting catalog tests passed\n");
  return 0;
}
