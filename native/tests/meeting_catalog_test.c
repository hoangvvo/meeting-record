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

static void BrowserMatches(void) {
  const struct {
    const char *url;
    mrec_platform platform;
  } services[] = {
      {"https://meet.google.com/abc-defg-hij", MREC_PLATFORM_MEET},
      {"https://teams.microsoft.com/l/meetup-join/123", MREC_PLATFORM_TEAMS},
      {"https://us02web.zoom.us/wc/123/join", MREC_PLATFORM_ZOOM},
      {"https://company.webex.com/meet/person", MREC_PLATFORM_WEBEX},
      {"https://app.slack.com/huddle/T01/C02", MREC_PLATFORM_SLACK},
      {"https://discord.com/channels/1/2", MREC_PLATFORM_DISCORD},
      {"https://whereby.com/room", MREC_PLATFORM_GENERIC_BROWSER},
      {"https://meet.jit.si/room", MREC_PLATFORM_GENERIC_BROWSER},
      {"https://app.gather.town/app/office", MREC_PLATFORM_GENERIC_BROWSER},
      {"https://around.co/room", MREC_PLATFORM_GENERIC_BROWSER},
  };
  const char *tabs[] = {"Inbox", "Meet - abc-defg-hij - Audio playing",
                         "https://teams.microsoft.com/l/meetup-join/123"};
  for (size_t index = 0; index < sizeof services / sizeof services[0]; index++) {
    mrec_browser_match match = mrec_catalog_browser_match(
        services[index].url, "Meeting", tabs, 3);
    assert(match.platform == (int32_t)services[index].platform);
    assert(match.tab_index == -1);
    assert(match.title_length == strlen("Meeting"));

    const char *background[] = {"Inbox", services[index].url};
    match = mrec_catalog_browser_match("https://example.com", "Inbox", background, 2);
    assert(match.platform == (int32_t)services[index].platform);
    assert(match.tab_index == 1);
    assert(match.title_length == strlen(services[index].url));
  }

  /* A URL wins over titles, including another service's background tab. */
  mrec_browser_match match = mrec_catalog_browser_match(
      "https://zoom.us/j/123", "Meet - abc-defg-hij - Browser", tabs, 3);
  assert(match.platform == MREC_PLATFORM_ZOOM);
  assert(match.tab_index == -1);
  assert(match.title_length == strlen("Meet - abc-defg-hij"));

  /* A window-title match keeps its URL; a tab match must omit that URL. */
  match = mrec_catalog_browser_match("https://example.com", "Meet - xyz-abcd-efg", tabs, 3);
  assert(match.platform == MREC_PLATFORM_MEET);
  assert(match.tab_index == -1);
  assert(match.title_length == strlen("Meet - xyz-abcd-efg"));
  match = mrec_catalog_browser_match("https://example.com", "Inbox", tabs, 3);
  assert(match.platform == MREC_PLATFORM_MEET);
  assert(match.tab_index == 1);
  assert(match.title_length == strlen("Meet - abc-defg-hij"));

  const char *unicode_tabs[] = {"Meet \xE2\x80\x93 abc-defg-hij - Browser"};
  match = mrec_catalog_browser_match(NULL, NULL, unicode_tabs, 1);
  assert(match.platform == MREC_PLATFORM_MEET);
  assert(match.tab_index == 0);
  assert(match.title_length == strlen("Meet \xE2\x80\x93 abc-defg-hij"));

  const char *nonmeetings[] = {NULL, "", "https://zoom.us/pricing", "Google Meet",
                               "https://meet.google.com/", "Music",
                               "Chat | Debt | Microsoft Teams",
                               "https://teams.microsoft.com/v2/"};
  match = mrec_catalog_browser_match("https://example.com", "Inbox", nonmeetings,
                                     sizeof nonmeetings / sizeof nonmeetings[0]);
  assert(match.platform == MREC_PLATFORM_UNKNOWN);
  assert(match.tab_index == -1);
  assert(match.title_length == strlen("Inbox"));
  match = mrec_catalog_browser_match(NULL, NULL, NULL, 0);
  assert(match.platform == MREC_PLATFORM_UNKNOWN);
  assert(match.tab_index == -1);
  assert(match.title_length == 0);
  match = mrec_catalog_browser_match("", "", NULL, 1);
  assert(match.platform == MREC_PLATFORM_UNKNOWN);
  assert(match.tab_index == -1);
  assert(match.title_length == 0);
}

static void NonMeetingURLs(void) {
  const char *urls[] = {
      "https://teams.microsoft.com/", "https://teams.microsoft.com/v2/",
      "https://teams.cloud.microsoft/v2/", "https://dod.teams.microsoft.us/",
      "https://teams.live.com/_#/conversations", "https://teams.office.com/calendar",
      "https://teams.microsoftonline.cn/l/chat/0/0", "https://teams.microsoft.com/l/meetup-join/",
      "https://teams.microsoft.com/meet/", "https://zoom.us/j/",
      "https://app.slack.com/huddle", "https://discord.com/channels",
      "https://whereby.com/", "https://meet.jit.si/", "https://app.gather.town/",
      "https://around.co/", "https://webex.com/meetings",
      "https://notzoom.us/j/123", "https://zoom.us.example.org/j/123",
      "https://notmeet.google.com/abc-defg-hij",
      "https://meet.google.com.evil.test/abc-defg-hij",
      "https://meet.google.com/abc-defg-hij.example",
      "https://teams.microsoft.com.evil.test/l/meetup-join/123",
      "https://example.org/teams.microsoft.com/l/meetup-join/123",
      "https://example.org/?next=https://zoom.us/j/123",
      "https://example.org/#https://meet.google.com/abc-defg-hij",
      "https://zoom.us@evil.test/j/123", "https://evil.test@zoom.us/j/123",
      "file://zoom.us/j/123", "Chat | Debt | Microsoft Teams",
  };
  for (size_t index = 0; index < sizeof urls / sizeof urls[0]; index++) {
    assert(mrec_catalog_url_platform(urls[index]) == MREC_PLATFORM_UNKNOWN);
    assert(mrec_catalog_title_platform(urls[index], NULL) == MREC_PLATFORM_UNKNOWN);
    mrec_browser_match match = mrec_catalog_browser_match(urls[index], "Inbox", NULL, 0);
    assert(match.platform == MREC_PLATFORM_UNKNOWN);
    assert(mrec_catalog_confidence(1, 1, 0, match.platform) == 0);
  }
}

static void CallStateEvidence(void) {
  const struct {
    int32_t platform;
    int32_t role;
    const char *identifier;
    const char *label;
  } joined[] = {
      {MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, "hangup-button", "Localized leave label"},
      {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, "", "Leave call"},
      {MREC_PLATFORM_ZOOM, MREC_CONTROL_MENU_ITEM, "", "Leave Meeting"},
      {MREC_PLATFORM_WEBEX, MREC_CONTROL_BUTTON, "", "Leave meeting"},
      {MREC_PLATFORM_SLACK, MREC_CONTROL_BUTTON, "", "Leave huddle"},
      {MREC_PLATFORM_GENERIC_BROWSER, MREC_CONTROL_BUTTON, "", "Leave call"},
  };
  for (size_t index = 0; index < sizeof joined / sizeof joined[0]; index++) {
    const uint32_t signals = mrec_catalog_call_control(joined[index].platform, joined[index].role,
                                                       joined[index].identifier, joined[index].label);
    const int32_t state = mrec_catalog_call_state(signals);
    assert(state == MREC_CALL_JOINED);
    /* Silent/listen-only calls are actionable without any audio stream. */
    assert(mrec_catalog_call_confidence(0, 0, 0, joined[index].platform, state) == 90);
    assert(mrec_catalog_call_confidence(1, 1, 0, joined[index].platform, state) == 95);
    assert(mrec_catalog_call_confidence(1, 1, 1, joined[index].platform, state) == 100);
    /* A text message quoting a control's label is not evidence. */
    assert(mrec_catalog_call_control(joined[index].platform, MREC_CONTROL_TEXT,
                                      joined[index].identifier, joined[index].label) == 0);
  }

  const struct { int32_t platform; const char *identifier; const char *label; } previews[] = {
      {MREC_PLATFORM_TEAMS, "prejoin-join-button", "Localized join label"},
      {MREC_PLATFORM_MEET, "", "Join now"},
      {MREC_PLATFORM_MEET, "", "Join now without camera"},
      {MREC_PLATFORM_MEET, "", "Ask to join"},
      {MREC_PLATFORM_MEET, "", "Rejoin"},
      {MREC_PLATFORM_MEET, "", "Switch here"},
      {MREC_PLATFORM_MEET, "", "Switch here without camera"},
      {MREC_PLATFORM_ZOOM, "", "Join meeting"},
      {MREC_PLATFORM_WEBEX, "", "Join meeting"},
      {MREC_PLATFORM_SLACK, "", "Join huddle"},
      {MREC_PLATFORM_DISCORD, "", "Join voice"},
      {MREC_PLATFORM_GENERIC_BROWSER, "", "Ask to join"},
  };
  for (size_t index = 0; index < sizeof previews / sizeof previews[0]; index++) {
    const int32_t state = mrec_catalog_call_state(mrec_catalog_call_control(
        previews[index].platform, MREC_CONTROL_BUTTON, previews[index].identifier, previews[index].label));
    assert(state == MREC_CALL_NOT_JOINED);
    assert(mrec_catalog_call_confidence(0, 1, 1, previews[index].platform, state) == 0);
    assert(mrec_catalog_call_confidence(1, 1, 1, previews[index].platform, state) == 0);
  }

  /* Discord's generic Disconnect control alone can belong to account settings. */
  const uint32_t disconnect = mrec_catalog_call_control(MREC_PLATFORM_DISCORD, MREC_CONTROL_BUTTON,
                                                       "", "Disconnect");
  const uint32_t voice = mrec_catalog_call_control(MREC_PLATFORM_DISCORD, MREC_CONTROL_TEXT,
                                                 "", "Voice Connected");
  assert(mrec_catalog_call_state(disconnect) == MREC_CALL_UNKNOWN);
  assert(mrec_catalog_call_state(voice) == MREC_CALL_UNKNOWN);
  assert(mrec_catalog_call_state(disconnect | voice) == MREC_CALL_JOINED);
  assert(mrec_catalog_call_control(MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, "", "Leave") == 0);
  assert(mrec_catalog_call_control(MREC_PLATFORM_UNKNOWN, MREC_CONTROL_BUTTON, "hangup-button", "Leave call") == 0);
  assert(mrec_catalog_call_control(MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, "not-hangup-button", "Cancel") == 0);
  assert(mrec_catalog_call_confidence(1, 1, 1, MREC_PLATFORM_UNKNOWN, MREC_CALL_JOINED) == 0);
  assert(mrec_catalog_call_confidence(0, 1, 1, MREC_PLATFORM_UNKNOWN, MREC_CALL_JOINED) == 0);

  const uint32_t lobby = mrec_catalog_call_control(MREC_PLATFORM_ZOOM, MREC_CONTROL_STATUS, "",
      "Please wait for the host to start this meeting.");
  assert(mrec_catalog_call_state(lobby | MREC_CALL_SIGNAL_JOINED) == MREC_CALL_NOT_JOINED);
  assert(mrec_catalog_call_control(MREC_PLATFORM_ZOOM, MREC_CONTROL_TEXT, "",
      "Please wait for the host to start this meeting.") == 0);
  assert(mrec_catalog_call_control(MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, "", "Join Audio") == 0);
  assert(mrec_catalog_call_state(MREC_CALL_SIGNAL_JOINED | MREC_CALL_SIGNAL_NOT_JOINED) == MREC_CALL_JOINED);

  /* Service identity permits probing /v2/ without making that URL a meeting. */
  assert(mrec_catalog_context_platform("https://teams.microsoft.com/v2/", "Chat") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://teams.microsoft.com/v2/") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_context_platform("", "Chat | Debt | Microsoft Teams") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_context_platform("https://example.com/", "Meet - abc-defg-hij") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_context_platform("https://example.com", "Meet - abc-defg-hij") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_context_platform("https://meet.google.com", "") == MREC_PLATFORM_MEET);
  assert(mrec_catalog_context_platform("file://somewhere", "Meet - abc-defg-hij") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_context_platform("https://teams.microsoft.com.evil.test/v2/", "") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_context_platform(NULL, NULL) == MREC_PLATFORM_UNKNOWN);
}

int main(void) {
  BrowserMatches();
  NonMeetingURLs();
  CallStateEvidence();
  /* macOS: bundle-id prefixes reach the helpers that carry the audio. */
  assert(mrec_catalog_bundle_platform("us.zoom.xos") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("us.zoom.xos.cpthost") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("com.microsoft.teams2") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_bundle_platform("com.microsoft.teams") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_bundle_platform("com.tinyspeck.slackmacgap.helper") ==
         MREC_PLATFORM_SLACK);
  assert(mrec_catalog_bundle_platform("US.ZOOM.XOS") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("us.zoom.ZoomHybridConf") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_bundle_platform("Cisco-Systems.Spark") == MREC_PLATFORM_WEBEX);
  assert(mrec_catalog_bundle_platform("com.cisco.webexmeetingsapp") == MREC_PLATFORM_WEBEX);
  assert(mrec_catalog_bundle_platform("us.zoom.unrelated") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_bundle_platform("com.microsoft.teamsbackup") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_bundle_platform("com.microsoft.teams2backup") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_bundle_platform("com.hnc.discordlike") == MREC_PLATFORM_UNKNOWN);
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
  assert(mrec_catalog_bundle_browser("com.google.chromelike") == NULL);
  assert(mrec_catalog_bundle_browser("org.mozilla.firefoxbackup") == NULL);

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
  assert(mrec_catalog_url_platform("https://meet.google.com/meet/abc-defg-hij") ==
         MREC_PLATFORM_MEET);
  assert(mrec_catalog_url_platform("https://meet.google.com/abc-defg-hij?authuser=1") ==
         MREC_PLATFORM_MEET);
  /* Meet needs a meeting code: its home page and lookups are not calls. */
  assert(mrec_catalog_url_platform("https://meet.google.com/") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://meet.google.com/landing") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://meet.google.com/a") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://meet.google.com/abc-defg-") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://meet.google.com/abc-defg-hijk") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_url_platform("https://teams.microsoft.com/l/meetup-join/1") ==
         MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://teams.cloud.microsoft/meet/123?p=passcode") ==
         MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://dod.teams.microsoft.us/l/meetup-join/123") ==
         MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://teams.live.com/meet/123") == MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("https://teams.microsoft.com/l/call/0/0?users=someone") ==
         MREC_PLATFORM_TEAMS);
  assert(mrec_catalog_url_platform("HTTPS://US02WEB.ZOOM.US:443/j/123") == MREC_PLATFORM_ZOOM);
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
  /* Other titles fall back to complete call links and stay whole. */
  assert(mrec_catalog_title_platform("Daily standup | meet.jit.si/room", NULL) ==
         MREC_PLATFORM_GENERIC_BROWSER);
  assert(PageTitleIs("Daily standup | meet.jit.si/room", "Daily standup | meet.jit.si/room"));
  assert(mrec_catalog_title_platform("Sync | meet.google.com/abc-defg-hij - Google Chrome", NULL) ==
         MREC_PLATFORM_MEET);
  assert(mrec_catalog_title_platform("Inbox - Outlook", NULL) == MREC_PLATFORM_UNKNOWN);
  assert(PageTitleIs("", ""));

  /* Native playback can be a notification, message, or video in any supported app. */
  const char *apps[] = {"zoom.exe", "ms-teams.exe", "webex.exe", "slack.exe", "discord.exe"};
  for (size_t index = 0; index < sizeof apps / sizeof apps[0]; index++) {
    const int32_t platform = mrec_catalog_executable_platform(apps[index]);
    assert(platform != MREC_PLATFORM_UNKNOWN);
    assert(mrec_catalog_confidence(0, 1, 0, platform) == 0);
    assert(mrec_catalog_confidence(0, 1, 1, platform) >= MREC_CATALOG_RECORD_THRESHOLD);
  }

  /* Browsers need a recognised call and the mic; native clients need the mic. */
  assert(mrec_catalog_confidence(0, 0, 0, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(0, 1, 0, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(0, 0, 1, MREC_PLATFORM_UNKNOWN) == 65);
  assert(mrec_catalog_confidence(0, 1, 1, MREC_PLATFORM_UNKNOWN) == 95);
  assert(mrec_catalog_confidence(0, 0, 1, MREC_PLATFORM_ZOOM) == 65);
  assert(mrec_catalog_confidence(1, 1, 0, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(1, 0, 0, MREC_PLATFORM_MEET) == 0);
  assert(mrec_catalog_confidence(1, 1, 0, MREC_PLATFORM_MEET) == 0);
  assert(mrec_catalog_confidence(1, 0, 1, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(1, 1, 1, MREC_PLATFORM_UNKNOWN) == 0);
  assert(mrec_catalog_confidence(1, 0, 1, MREC_PLATFORM_MEET) == MREC_CATALOG_RECORD_THRESHOLD);
  assert(mrec_catalog_confidence(1, 1, 1, MREC_PLATFORM_MEET) == 100);

  const int32_t browser_platforms[] = {MREC_PLATFORM_ZOOM, MREC_PLATFORM_TEAMS,
      MREC_PLATFORM_MEET, MREC_PLATFORM_WEBEX, MREC_PLATFORM_SLACK,
      MREC_PLATFORM_DISCORD, MREC_PLATFORM_GENERIC_BROWSER};
  for (size_t index = 0; index < sizeof browser_platforms / sizeof browser_platforms[0]; index++) {
    assert(mrec_catalog_confidence(1, 1, 0, browser_platforms[index]) == 0);
    assert(mrec_catalog_confidence(1, 1, 1, browser_platforms[index]) == 100);
  }

  /* Identifiers are stable. */
  assert(strcmp(mrec_platform_id(MREC_PLATFORM_MEET), "meet") == 0);
  assert(strcmp(mrec_platform_id(MREC_PLATFORM_GENERIC_BROWSER), "browser") == 0);
  assert(strcmp(mrec_platform_id((mrec_platform)99), "unknown") == 0);

  assert(mrec_catalog_linux_platform("zoom") == MREC_PLATFORM_ZOOM);
  assert(mrec_catalog_linux_platform("slack") == MREC_PLATFORM_SLACK);
  assert(mrec_catalog_linux_platform("Discord") == MREC_PLATFORM_DISCORD);
  assert(mrec_catalog_linux_platform("zoom-helper-lookalike") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_linux_platform("zoom.exe") == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_linux_platform(NULL) == MREC_PLATFORM_UNKNOWN);
  assert(mrec_catalog_linux_browser("chrome") != NULL);
  assert(mrec_catalog_linux_browser("firefox-bin") != NULL);
  assert(mrec_catalog_linux_browser("chromium-browser") != NULL);
  assert(mrec_catalog_linux_browser("chrome-malware") == NULL);
  assert(mrec_catalog_linux_browser(NULL) == NULL);
  printf("meeting catalog tests passed\n");
  return 0;
}
