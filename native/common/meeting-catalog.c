#include "meeting-catalog.h"
#include "meeting-record-detect.h"

#include <string.h>

typedef struct {
  const char *identifier; /* lowercased */
  mrec_platform platform;
} Identifier;

/* macOS bundle-id prefixes. Audio usually plays from a helper with a suffixed id. */
static const Identifier kBundlePrefixes[] = {
    {"us.zoom.", MREC_PLATFORM_ZOOM},
    /* `teams2` is current; `com.microsoft.teams` exists on older installs. */
    {"com.microsoft.teams", MREC_PLATFORM_TEAMS},
    {"com.cisco.webex", MREC_PLATFORM_WEBEX},
    {"com.webex.meetingmanager", MREC_PLATFORM_WEBEX},
    {"com.tinyspeck.slackmacgap", MREC_PLATFORM_SLACK},
    {"com.hnc.discord", MREC_PLATFORM_DISCORD},
};

/* Windows executable names, including the hosts that carry the audio. */
static const Identifier kExecutables[] = {
    {"zoom.exe", MREC_PLATFORM_ZOOM},
    {"cpthost.exe", MREC_PLATFORM_ZOOM}, /* screen-share helper */
    {"teams.exe", MREC_PLATFORM_TEAMS},
    {"ms-teams.exe", MREC_PLATFORM_TEAMS},
    {"ms-teams_modulehost.exe", MREC_PLATFORM_TEAMS}, /* new Teams media host */
    {"msteamsvdi.exe", MREC_PLATFORM_TEAMS},
    {"msteams.exe", MREC_PLATFORM_TEAMS},
    {"zoomhybridconf.exe", MREC_PLATFORM_ZOOM},
    {"webexmta.exe", MREC_PLATFORM_WEBEX},
    {"webex.exe", MREC_PLATFORM_WEBEX},
    {"atmgr.exe", MREC_PLATFORM_WEBEX},
    {"ciscocollabhost.exe", MREC_PLATFORM_WEBEX},
    {"slack.exe", MREC_PLATFORM_SLACK},
    {"discord.exe", MREC_PLATFORM_DISCORD},
};

/*
 * Browsers whose tabs may host a call. They count as a meeting only once a URL
 * or title matches, or the microphone is live.
 */
static const char *const kBrowserBundlePrefixes[] = {
    "com.google.chrome",       "org.chromium.chromium",      "com.apple.safari",
    "com.microsoft.edgemac",   "org.mozilla.firefox",        "company.thebrowser.browser",
    "com.brave.browser",       "com.vivaldi.vivaldi",        "com.operasoftware.opera",
    "ai.perplexity.comet",     "app.zen-browser.zen",
};

static const char *const kBrowserExecutables[] = {
    "chrome.exe",  "chromium.exe", "msedge.exe", "firefox.exe", "brave.exe",
    "arc.exe",     "vivaldi.exe",  "opera.exe",  "comet.exe",   "zen.exe",
};

/* URL fragments that identify a live call. Titles often carry them too. */
static const Identifier kUrlPatterns[] = {
    {"teams.microsoft.com", MREC_PLATFORM_TEAMS},
    {"teams.cloud.microsoft", MREC_PLATFORM_TEAMS},
    {"teams.office.com", MREC_PLATFORM_TEAMS},
    {"teams.live.com", MREC_PLATFORM_TEAMS},
    {"teams.microsoft.us", MREC_PLATFORM_TEAMS}, /* GCC High and DoD */
    {"teams.microsoftonline.cn", MREC_PLATFORM_TEAMS},
    {"teams.sovcloud.", MREC_PLATFORM_TEAMS},
    {"zoom.us/j/", MREC_PLATFORM_ZOOM},
    {"zoom.us/s/", MREC_PLATFORM_ZOOM},
    {"zoom.us/w/", MREC_PLATFORM_ZOOM},
    {"zoom.us/wc/", MREC_PLATFORM_ZOOM},
    {"zoomgov.com/j/", MREC_PLATFORM_ZOOM},
    {"zoomgov.com/s/", MREC_PLATFORM_ZOOM},
    {"zoomgov.com/w/", MREC_PLATFORM_ZOOM},
    {"zoomgov.com/wc/", MREC_PLATFORM_ZOOM},
    {"/webex.com/meet", MREC_PLATFORM_WEBEX},
    {".webex.com/meet", MREC_PLATFORM_WEBEX},
    {"webex.com/wbxmjs", MREC_PLATFORM_WEBEX},
    {"app.slack.com/huddle", MREC_PLATFORM_SLACK},
    {"discord.com/channels", MREC_PLATFORM_DISCORD},
    {"whereby.com/", MREC_PLATFORM_GENERIC_BROWSER},
    {"meet.jit.si", MREC_PLATFORM_GENERIC_BROWSER},
    {"app.gather.town", MREC_PLATFORM_GENERIC_BROWSER},
    {"around.co/", MREC_PLATFORM_GENERIC_BROWSER},
};

/* A known app is not a meeting on its own. */
enum {
  kScoreKnownApp = 40,
  kScorePlayingAudio = 30,
  kScoreUsingMic = 25,
  kScoreRecognised = 5,
};

enum { kMeetCodeLength = 12 }; /* abc-defg-hij */

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* ASCII only, so the host's locale never changes a match. */
static char Lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int IsLowerLetter(char c) { return c >= 'a' && c <= 'z'; }

static int IsAlphanumeric(char c) {
  return IsLowerLetter(Lower(c)) || (c >= '0' && c <= '9');
}

/* Case-insensitive; `prefix` is lowercased. */
static int HasPrefix(const char *text, const char *prefix) {
  for (; *prefix; text++, prefix++) {
    if (Lower(*text) != *prefix) return 0;
  }
  return 1;
}

static int IsEqual(const char *text, const char *lowered) {
  return HasPrefix(text, lowered) && text[strlen(lowered)] == '\0';
}

static int Contains(const char *text, const char *lowered) {
  for (; *text; text++) {
    if (HasPrefix(text, lowered)) return 1;
  }
  return 0;
}

/* A whole Meet meeting code: three, four and three lowercase letters. */
static int IsMeetCode(const char *text) {
  for (size_t index = 0; index < kMeetCodeLength; index++) {
    if (!text[index]) return 0;
    const int separator = index == 3 || index == 8;
    if (separator ? text[index] != '-' : !IsLowerLetter(text[index])) return 0;
  }
  return !IsAlphanumeric(text[kMeetCodeLength]);
}

/* A call link, not Meet's home page. */
static int ContainsMeetLink(const char *text) {
  static const char kHost[] = "meet.google.com/";
  for (; *text; text++) {
    if (HasPrefix(text, kHost) && IsMeetCode(text + sizeof kHost - 1)) return 1;
  }
  return 0;
}

/* "Meet - abc-defg-hij", with any dash, starts a Meet call's title. */
static size_t MeetTitleLength(const char *title) {
  if (strncmp(title, "Meet ", 5) != 0) return 0;
  const char *cursor = title + 5;
  while (*cursor == ' ') cursor++;
  if (*cursor == '-') {
    cursor++;
  } else if (strncmp(cursor, "\xE2\x80\x93", 3) == 0 ||
             strncmp(cursor, "\xE2\x80\x94", 3) == 0) {
    cursor += 3; /* en and em dashes */
  } else {
    return 0;
  }
  while (*cursor == ' ') cursor++;
  if (!IsMeetCode(cursor)) return 0;
  return (size_t)(cursor - title) + kMeetCodeLength;
}

int32_t mrec_catalog_bundle_platform(const char *bundle_id) {
  if (!bundle_id || !*bundle_id) return MREC_PLATFORM_UNKNOWN;
  for (size_t index = 0; index < COUNT(kBundlePrefixes); index++) {
    if (HasPrefix(bundle_id, kBundlePrefixes[index].identifier)) {
      return kBundlePrefixes[index].platform;
    }
  }
  return MREC_PLATFORM_UNKNOWN;
}

const char *mrec_catalog_bundle_browser(const char *bundle_id) {
  if (!bundle_id || !*bundle_id) return NULL;
  for (size_t index = 0; index < COUNT(kBrowserBundlePrefixes); index++) {
    if (HasPrefix(bundle_id, kBrowserBundlePrefixes[index])) {
      return kBrowserBundlePrefixes[index];
    }
  }
  return NULL;
}

int32_t mrec_catalog_executable_platform(const char *executable) {
  if (!executable) return MREC_PLATFORM_UNKNOWN;
  for (size_t index = 0; index < COUNT(kExecutables); index++) {
    if (IsEqual(executable, kExecutables[index].identifier)) {
      return kExecutables[index].platform;
    }
  }
  return MREC_PLATFORM_UNKNOWN;
}

const char *mrec_catalog_executable_browser(const char *executable) {
  if (!executable) return NULL;
  for (size_t index = 0; index < COUNT(kBrowserExecutables); index++) {
    if (IsEqual(executable, kBrowserExecutables[index])) return kBrowserExecutables[index];
  }
  return NULL;
}

int32_t mrec_catalog_url_platform(const char *url) {
  if (!url) return MREC_PLATFORM_UNKNOWN;
  if (ContainsMeetLink(url)) return MREC_PLATFORM_MEET;
  for (size_t index = 0; index < COUNT(kUrlPatterns); index++) {
    if (Contains(url, kUrlPatterns[index].identifier)) return kUrlPatterns[index].platform;
  }
  return MREC_PLATFORM_UNKNOWN;
}

int32_t mrec_catalog_title_platform(const char *title, size_t *page_length) {
  if (!title) title = "";
  const size_t meet = MeetTitleLength(title);
  if (page_length) *page_length = meet ? meet : strlen(title);
  return meet ? MREC_PLATFORM_MEET : mrec_catalog_url_platform(title);
}

mrec_browser_match mrec_catalog_browser_match(const char *url, const char *title,
                                             const char *const *tab_titles,
                                             size_t tab_count) {
  mrec_browser_match match = {MREC_PLATFORM_UNKNOWN, 0, -1};
  const int32_t title_platform = mrec_catalog_title_platform(title, &match.title_length);
  match.platform = mrec_catalog_url_platform(url);
  if (match.platform != MREC_PLATFORM_UNKNOWN) return match;
  match.platform = title_platform;
  if (match.platform != MREC_PLATFORM_UNKNOWN) return match;
  if (!tab_titles) return match;
  for (size_t index = 0; index < tab_count && index <= INT32_MAX; index++) {
    size_t length = 0;
    const int32_t platform = mrec_catalog_title_platform(tab_titles[index], &length);
    if (platform == MREC_PLATFORM_UNKNOWN) continue;
    match.platform = platform;
    match.title_length = length;
    match.tab_index = (int32_t)index;
    return match;
  }
  return match;
}

int32_t mrec_catalog_confidence(int32_t is_browser, int32_t playing_audio,
                                int32_t using_mic, int32_t recognised) {
  const int matched = is_browser && recognised != MREC_PLATFORM_UNKNOWN;
  if (!playing_audio && !using_mic) return 0;
  /* A video tab and a call tab look alike at the process level. */
  if (is_browser && !matched && !using_mic) return 0;
  return kScoreKnownApp + (playing_audio ? kScorePlayingAudio : 0) +
         (using_mic ? kScoreUsingMic : 0) + (matched ? kScoreRecognised : 0);
}

const char *mrec_platform_id(mrec_platform platform) {
  switch (platform) {
    case MREC_PLATFORM_ZOOM: return "zoom";
    case MREC_PLATFORM_TEAMS: return "teams";
    case MREC_PLATFORM_MEET: return "meet";
    case MREC_PLATFORM_WEBEX: return "webex";
    case MREC_PLATFORM_SLACK: return "slack";
    case MREC_PLATFORM_DISCORD: return "discord";
    case MREC_PLATFORM_GENERIC_BROWSER: return "browser";
    case MREC_PLATFORM_UNKNOWN: break;
  }
  return "unknown";
}
