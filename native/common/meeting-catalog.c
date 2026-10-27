#include "meeting-catalog.h"
#include "meeting-record-detect.h"

#include <string.h>

typedef struct {
  const char *identifier; /* lowercased */
  mrec_platform platform;
} Identifier;

/* macOS bundle-id prefixes. Audio usually plays from a helper with a suffixed id. */
static const Identifier kBundlePrefixes[] = {
    {"us.zoom.xos", MREC_PLATFORM_ZOOM},
    {"us.zoom.zoomhybridconf", MREC_PLATFORM_ZOOM},
    {"us.zoom.zoomphone", MREC_PLATFORM_ZOOM},
    {"us.zoom.cpthost", MREC_PLATFORM_ZOOM},
    /* `teams2` is current; `com.microsoft.teams` exists on older installs. */
    {"com.microsoft.teams", MREC_PLATFORM_TEAMS},
    {"com.microsoft.teams2", MREC_PLATFORM_TEAMS},
    {"com.cisco.webex", MREC_PLATFORM_WEBEX},
    {"com.cisco.webexmeetingsapp", MREC_PLATFORM_WEBEX},
    {"cisco-systems.spark", MREC_PLATFORM_WEBEX},
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
 * Browsers whose tabs may host a call. They need both a call URL/title and
 * microphone activity.
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

/* A Teams home/chat/calendar URL does not identify a call. */
static const char *const kTeamsHosts[] = {
    "teams.microsoft.com", "teams.cloud.microsoft", "teams.office.com",
    "teams.live.com", "teams.microsoft.us", "teams.microsoftonline.cn",
};

typedef struct {
  const char *host;
  const char *path; /* a call route followed by a nonempty room/id */
  int subdomains;
  mrec_platform platform;
} URLPattern;

static const URLPattern kUrlPatterns[] = {
    {"zoom.us", "/j/", 1, MREC_PLATFORM_ZOOM},
    {"zoom.us", "/s/", 1, MREC_PLATFORM_ZOOM},
    {"zoom.us", "/w/", 1, MREC_PLATFORM_ZOOM},
    {"zoom.us", "/wc/", 1, MREC_PLATFORM_ZOOM},
    {"zoomgov.com", "/j/", 1, MREC_PLATFORM_ZOOM},
    {"zoomgov.com", "/s/", 1, MREC_PLATFORM_ZOOM},
    {"zoomgov.com", "/w/", 1, MREC_PLATFORM_ZOOM},
    {"zoomgov.com", "/wc/", 1, MREC_PLATFORM_ZOOM},
    {"webex.com", "/meet/", 1, MREC_PLATFORM_WEBEX},
    {"webex.com", "/wbxmjs/", 1, MREC_PLATFORM_WEBEX},
    {"app.slack.com", "/huddle/", 0, MREC_PLATFORM_SLACK},
    {"discord.com", "/channels/", 0, MREC_PLATFORM_DISCORD},
    {"whereby.com", "/", 0, MREC_PLATFORM_GENERIC_BROWSER},
    {"meet.jit.si", "/", 0, MREC_PLATFORM_GENERIC_BROWSER},
    {"app.gather.town", "/app/", 0, MREC_PLATFORM_GENERIC_BROWSER},
    {"around.co", "/", 0, MREC_PLATFORM_GENERIC_BROWSER},
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

/* Helpers/variants have a dotted suffix, not an arbitrary lookalike name. */
static int IdentifierMatches(const char *text, const char *prefix) {
  if (!HasPrefix(text, prefix)) return 0;
  const size_t length = strlen(prefix);
  return text[length] == '\0' || text[length] == '.';
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

typedef struct {
  const char *host;
  size_t host_length;
  const char *path;
  size_t path_length;
} URL;

/* Also accepts scheme-less addresses exposed by browser address bars. */
static int ParseURL(const char *text, URL *url) {
  if (HasPrefix(text, "https://")) text += 8;
  else if (HasPrefix(text, "http://")) text += 7;

  url->host = text;
  while (IsAlphanumeric(*text) || *text == '-' || *text == '.') text++;
  url->host_length = (size_t)(text - url->host);
  if (!url->host_length) return 0;
  if (*text == ':') {
    text++;
    const char *port = text;
    while (*text >= '0' && *text <= '9') text++;
    if (text == port) return 0;
  }
  /* Reject credentials, lookalike host suffixes, and unsupported schemes. */
  if (*text && *text != '/' && *text != '?' && *text != '#') return 0;
  url->path = *text == '/' ? text : "";
  url->path_length = strcspn(url->path, "?# \t\r\n\"'<>|()[]");
  return 1;
}

static int HostMatches(const URL *url, const char *host, int subdomains) {
  const size_t length = strlen(host);
  if (url->host_length < length) return 0;
  const size_t offset = url->host_length - length;
  if (offset && (!subdomains || url->host[offset - 1] != '.')) return 0;
  return HasPrefix(url->host + offset, host);
}

static int HasRoom(const URL *url, const char *path) {
  const size_t length = strlen(path);
  return url->path_length > length && HasPrefix(url->path, path) &&
         url->path[length] != '/';
}

static int32_t URLPlatform(const URL *url) {
  if (HostMatches(url, "meet.google.com", 0)) {
    const size_t prefix = HasPrefix(url->path, "/meet/") ? 6 : 1;
    if (url->path_length == kMeetCodeLength + prefix && IsMeetCode(url->path + prefix)) {
      return MREC_PLATFORM_MEET;
    }
  }
  for (size_t index = 0; index < COUNT(kTeamsHosts); index++) {
    if (HostMatches(url, kTeamsHosts[index], 1) &&
        (HasRoom(url, "/l/meetup-join/") || HasRoom(url, "/meet/") ||
         HasRoom(url, "/l/call/"))) {
      return MREC_PLATFORM_TEAMS;
    }
  }
  for (size_t index = 0; index < COUNT(kUrlPatterns); index++) {
    const URLPattern *pattern = &kUrlPatterns[index];
    if (HostMatches(url, pattern->host, pattern->subdomains) && HasRoom(url, pattern->path)) {
      return pattern->platform;
    }
  }
  return MREC_PLATFORM_UNKNOWN;
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
    if (IdentifierMatches(bundle_id, kBundlePrefixes[index].identifier)) {
      return kBundlePrefixes[index].platform;
    }
  }
  return MREC_PLATFORM_UNKNOWN;
}

const char *mrec_catalog_bundle_browser(const char *bundle_id) {
  if (!bundle_id || !*bundle_id) return NULL;
  for (size_t index = 0; index < COUNT(kBrowserBundlePrefixes); index++) {
    if (IdentifierMatches(bundle_id, kBrowserBundlePrefixes[index])) {
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
  URL parsed;
  return ParseURL(url, &parsed) ? URLPlatform(&parsed) : MREC_PLATFORM_UNKNOWN;
}

int32_t mrec_catalog_context_platform(const char *url, const char *title) {
  URL parsed;
  if (url && *url) {
    if (!ParseURL(url, &parsed)) return MREC_PLATFORM_UNKNOWN;
    if (HostMatches(&parsed, "meet.google.com", 0)) return MREC_PLATFORM_MEET;
    for (size_t index = 0; index < COUNT(kTeamsHosts); index++) {
      if (HostMatches(&parsed, kTeamsHosts[index], 1)) return MREC_PLATFORM_TEAMS;
    }
    for (size_t index = 0; index < COUNT(kUrlPatterns); index++) {
      if (HostMatches(&parsed, kUrlPatterns[index].host, kUrlPatterns[index].subdomains)) {
        return kUrlPatterns[index].platform;
      }
    }
    /* An unrelated active URL must not borrow identity from a stale title/tab. */
    return MREC_PLATFORM_UNKNOWN;
  }
  if (!title) return MREC_PLATFORM_UNKNOWN;
  if (MeetTitleLength(title)) return MREC_PLATFORM_MEET;
  const size_t length = strlen(title);
  static const char *const teams_suffixes[] = {" | microsoft teams", " - microsoft teams"};
  for (size_t index = 0; index < COUNT(teams_suffixes); index++) {
    const size_t suffix = strlen(teams_suffixes[index]);
    if (length >= suffix && IsEqual(title + length - suffix, teams_suffixes[index])) {
      return MREC_PLATFORM_TEAMS;
    }
  }
  return MREC_PLATFORM_UNKNOWN;
}

typedef struct {
  int32_t platform;
  int32_t role;
  const char *identifier;
  const char *label; /* ASCII case-insensitive; exact accessible control name */
  uint32_t signal;
} CallControl;

/* Match controls, never arbitrary document text or page-title substrings. */
static const CallControl kCallControls[] = {
    {MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, "hangup-button", NULL, MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, "prejoin-join-button", NULL, MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, NULL, "join now", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_TEAMS, MREC_CONTROL_BUTTON, NULL, "rejoin", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "leave call", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "join now", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "ask to join", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "rejoin", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "end meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "end meeting for all", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_MENU_ITEM, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_MENU_ITEM, NULL, "end meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "join meeting", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_WEBEX, MREC_CONTROL_BUTTON, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_WEBEX, MREC_CONTROL_BUTTON, NULL, "end meeting for all", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_WEBEX, MREC_CONTROL_MENU_ITEM, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_WEBEX, MREC_CONTROL_BUTTON, NULL, "join meeting", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_SLACK, MREC_CONTROL_BUTTON, NULL, "leave huddle", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_SLACK, MREC_CONTROL_BUTTON, NULL, "join huddle", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_DISCORD, MREC_CONTROL_BUTTON, NULL, "disconnect", MREC_CALL_SIGNAL_DISCONNECT},
    {MREC_PLATFORM_DISCORD, MREC_CONTROL_TEXT, NULL, "voice connected", MREC_CALL_SIGNAL_VOICE_CONNECTED},
    {MREC_PLATFORM_DISCORD, MREC_CONTROL_BUTTON, NULL, "voice connected", MREC_CALL_SIGNAL_VOICE_CONNECTED},
    {MREC_PLATFORM_DISCORD, MREC_CONTROL_BUTTON, NULL, "join voice", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_GENERIC_BROWSER, MREC_CONTROL_BUTTON, NULL, "leave call", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_GENERIC_BROWSER, MREC_CONTROL_BUTTON, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_GENERIC_BROWSER, MREC_CONTROL_BUTTON, NULL, "join meeting", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_GENERIC_BROWSER, MREC_CONTROL_BUTTON, NULL, "ask to join", MREC_CALL_SIGNAL_NOT_JOINED},
    /* Status/heading context is required, so chat messages cannot veto a call. */
    {MREC_PLATFORM_TEAMS, MREC_CONTROL_STATUS, NULL, "we've let people know you're waiting", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_TEAMS, MREC_CONTROL_STATUS, NULL, "when the meeting starts, we'll let people know you're waiting", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_STATUS, NULL, "asking to be let in", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_STATUS, NULL, "waiting for the host to let you in", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_STATUS, NULL, "you left the meeting", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_STATUS, NULL, "please wait for the host to start this meeting", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_STATUS, NULL, "please wait, the meeting host will let you in soon", MREC_CALL_SIGNAL_BLOCKED},
    {MREC_PLATFORM_WEBEX, MREC_CONTROL_STATUS, NULL, "waiting for the host", MREC_CALL_SIGNAL_BLOCKED},
};

static int LabelEqual(const char *text, const char *label) {
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
  size_t length = strlen(text);
  while (length && strchr(" \t\r\n.", text[length - 1])) length--;
  return length == strlen(label) && HasPrefix(text, label);
}

uint32_t mrec_catalog_call_control(int32_t platform, int32_t role,
                                 const char *identifier, const char *label) {
  uint32_t signals = 0;
  for (size_t index = 0; index < COUNT(kCallControls); index++) {
    const CallControl *control = &kCallControls[index];
    if (control->platform != platform || control->role != role) continue;
    if ((control->identifier && identifier && IsEqual(identifier, control->identifier)) ||
        (control->label && label && LabelEqual(label, control->label))) {
      signals |= control->signal;
    }
  }
  return signals;
}

int32_t mrec_catalog_call_state(uint32_t signals) {
  if (signals & MREC_CALL_SIGNAL_BLOCKED) return MREC_CALL_NOT_JOINED;
  const uint32_t voice = MREC_CALL_SIGNAL_DISCONNECT | MREC_CALL_SIGNAL_VOICE_CONNECTED;
  if ((signals & MREC_CALL_SIGNAL_JOINED) || (signals & voice) == voice) return MREC_CALL_JOINED;
  if (signals & MREC_CALL_SIGNAL_NOT_JOINED) return MREC_CALL_NOT_JOINED;
  return MREC_CALL_UNKNOWN;
}

int32_t mrec_catalog_title_platform(const char *title, size_t *page_length) {
  if (!title) title = "";
  const size_t meet = MeetTitleLength(title);
  if (page_length) *page_length = meet ? meet : strlen(title);
  if (meet) return MREC_PLATFORM_MEET;
  /* A title may contain a link, but a host inside another URL is not a link. */
  for (const char *cursor = title; *cursor; cursor++) {
    if (cursor != title && !strchr(" \t\r\n\"'<>|([", cursor[-1])) continue;
    const int32_t platform = mrec_catalog_url_platform(cursor);
    if (platform != MREC_PLATFORM_UNKNOWN) return platform;
  }
  return MREC_PLATFORM_UNKNOWN;
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
  return mrec_catalog_call_confidence(is_browser, playing_audio, using_mic, recognised,
                                      MREC_CALL_UNKNOWN);
}

int32_t mrec_catalog_call_confidence(int32_t is_browser, int32_t playing_audio,
                                   int32_t using_mic, int32_t recognised,
                                   int32_t call_state) {
  const int matched = is_browser && recognised != MREC_PLATFORM_UNKNOWN;
  if (call_state == MREC_CALL_NOT_JOINED) return 0;
  if (call_state == MREC_CALL_JOINED) {
    if (recognised == MREC_PLATFORM_UNKNOWN) return 0;
    return 90 + (playing_audio ? 5 : 0) + (using_mic ? 5 : 0);
  }
  /* Playback may be a message/video, even with a stale meeting tab open. */
  if (!using_mic) return 0;
  /* Dictation and microphone tests on unrelated sites are not browser meetings. */
  if (is_browser && !matched) return 0;
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
