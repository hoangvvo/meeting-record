#include "meeting-catalog.h"
#include "meeting-record-detect.h"

#include <string.h>

typedef struct {
  const char *identifier; /* macOS/Windows: lowercased; Linux: exact */
  mrec_platform platform;
} Identifier;

static const Identifier kLinuxExecutables[] = {
    {"zoom", MREC_PLATFORM_ZOOM},
    {"ZoomLauncher", MREC_PLATFORM_ZOOM},
    {"slack", MREC_PLATFORM_SLACK},
    {"Discord", MREC_PLATFORM_DISCORD},
    {"discord", MREC_PLATFORM_DISCORD},
    {"DiscordCanary", MREC_PLATFORM_DISCORD},
    {"DiscordPTB", MREC_PLATFORM_DISCORD},
    {"webex", MREC_PLATFORM_WEBEX},
    {"Webex", MREC_PLATFORM_WEBEX},
    {"tuple", MREC_PLATFORM_TUPLE},
    {"lark", MREC_PLATFORM_LARK},
    {"feishu", MREC_PLATFORM_LARK},
    {"Telegram", MREC_PLATFORM_TELEGRAM},
    {"signal-desktop", MREC_PLATFORM_SIGNAL},
    {"Viber", MREC_PLATFORM_VIBER},
    {"wechat", MREC_PLATFORM_WECHAT},
    {"jitsi-meet", MREC_PLATFORM_JITSI},
    {"element-desktop", MREC_PLATFORM_ELEMENT},
    {"element-desktop-nightly", MREC_PLATFORM_ELEMENT},
    {"com.Mattermost.Desktop", MREC_PLATFORM_MATTERMOST},
    {"Nextcloud Talk", MREC_PLATFORM_NEXTCLOUD_TALK},
    {"jami", MREC_PLATFORM_JAMI},
    {"wemeetapp", MREC_PLATFORM_TENCENT_MEETING},
};

int32_t mrec_catalog_linux_platform(const char *name) {
  if (!name) return MREC_PLATFORM_UNKNOWN;
  for (size_t i = 0; i < sizeof kLinuxExecutables / sizeof kLinuxExecutables[0]; ++i)
    if (!strcmp(name, kLinuxExecutables[i].identifier)) return kLinuxExecutables[i].platform;
  return MREC_PLATFORM_UNKNOWN;
}

const char *mrec_catalog_linux_browser(const char *name) {
  static const char *const browsers[] = {
      "chrome", "google-chrome", "google-chrome-stable", "chromium", "chromium-browser",
      "firefox", "firefox-bin", "brave", "brave-browser", "msedge", "microsoft-edge",
      "vivaldi-bin", "opera", "zen", "zen-bin", "helium", "waterfox", "whale"};
  if (!name) return NULL;
  for (size_t i = 0; i < sizeof browsers / sizeof browsers[0]; ++i)
    if (!strcmp(name, browsers[i])) return browsers[i];
  return NULL;
}

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
    {"com.logmein.goto", MREC_PLATFORM_GOTO},
    {"com.electron.dialpad", MREC_PLATFORM_DIALPAD},
    {"com.electron.uberconference", MREC_PLATFORM_DIALPAD},
    {"net.whatsapp.whatsapp", MREC_PLATFORM_WHATSAPP},
    {"io.aircall.phone", MREC_PLATFORM_AIRCALL},
    {"io.aircall.workspace", MREC_PLATFORM_AIRCALL},
    {"com.ringcentral.glip", MREC_PLATFORM_RINGCENTRAL},
    {"com.electron.8x8---virtual-office", MREC_PLATFORM_8X8},
    {"com.tencent.tencentmeeting", MREC_PLATFORM_VOOV},
    {"app.tuple.app", MREC_PLATFORM_TUPLE},
    {"com.gather.gather", MREC_PLATFORM_GATHER},
    {"com.gather.gatherv2", MREC_PLATFORM_GATHER},
    {"com.larksuite.larkapp", MREC_PLATFORM_LARK},
    {"com.electron.lark", MREC_PLATFORM_LARK},
    {"com.bytedance.macos.feishu", MREC_PLATFORM_LARK},
    {"inc.wonder.roam", MREC_PLATFORM_ROAM},
    {"com.clickup.desktop-app", MREC_PLATFORM_CLICKUP},
    {"ru.yandex.desktop.telemost", MREC_PLATFORM_TELEMOST},
    /* Installed Meet PWAs are standalone app windows, not browser tabs. */
    {"com.google.chrome.app.kjgfgldnnfoeklkmfkjfagphfepbbdan", MREC_PLATFORM_MEET},
    {"com.google.chrome.beta.app.kjgfgldnnfoeklkmfkjfagphfepbbdan", MREC_PLATFORM_MEET},
    {"company.thebrowser.dia.app.kjgfgldnnfoeklkmfkjfagphfepbbdan", MREC_PLATFORM_MEET},
    {"ru.keepcoder.telegram", MREC_PLATFORM_TELEGRAM},
    {"org.telegram.desktop", MREC_PLATFORM_TELEGRAM},
    {"com.tdesktop.telegram", MREC_PLATFORM_TELEGRAM},
    {"org.whispersystems.signal-desktop", MREC_PLATFORM_SIGNAL},
    {"com.viber.osx", MREC_PLATFORM_VIBER},
    {"jp.naver.line.mac", MREC_PLATFORM_LINE},
    {"com.tencent.xinwechat", MREC_PLATFORM_WECHAT},
    {"com.vng.zalo", MREC_PLATFORM_ZALO},
    {"org.jitsi.jitsi-meet", MREC_PLATFORM_JITSI},
    {"im.riot.app", MREC_PLATFORM_ELEMENT},
    {"im.riot.nightly", MREC_PLATFORM_ELEMENT},
    {"mattermost.desktop", MREC_PLATFORM_MATTERMOST},
    {"com.nextcloud.talk.mac", MREC_PLATFORM_NEXTCLOUD_TALK},
    {"com.savoirfairelinux.ring.macos", MREC_PLATFORM_JAMI},
    {"cx.ring", MREC_PLATFORM_JAMI},
    {"com.tencent.meeting", MREC_PLATFORM_TENCENT_MEETING},
    {"com.tencent.weworkmac", MREC_PLATFORM_WECOM},
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
    {"goto.exe", MREC_PLATFORM_GOTO},
    {"dialpad.exe", MREC_PLATFORM_DIALPAD},
    {"dialpadmeetings.exe", MREC_PLATFORM_DIALPAD},
    {"whatsapp.exe", MREC_PLATFORM_WHATSAPP},
    {"whatsapp.root.exe", MREC_PLATFORM_WHATSAPP},
    {"aircall.exe", MREC_PLATFORM_AIRCALL},
    {"aircall workspace.exe", MREC_PLATFORM_AIRCALL},
    {"aircallworkspace.exe", MREC_PLATFORM_AIRCALL},
    {"ringcentral.exe", MREC_PLATFORM_RINGCENTRAL},
    {"8x8 work.exe", MREC_PLATFORM_8X8},
    {"voovmeetingapp.exe", MREC_PLATFORM_VOOV},
    {"tuple.exe", MREC_PLATFORM_TUPLE},
    {"gather.exe", MREC_PLATFORM_GATHER},
    {"gatherv2.exe", MREC_PLATFORM_GATHER},
    {"lark.exe", MREC_PLATFORM_LARK},
    {"roam.exe", MREC_PLATFORM_ROAM},
    {"clickup.exe", MREC_PLATFORM_CLICKUP},
    {"yandextelemost.exe", MREC_PLATFORM_TELEMOST},
    {"telegram.exe", MREC_PLATFORM_TELEGRAM},
    {"signal.exe", MREC_PLATFORM_SIGNAL},
    {"viber.exe", MREC_PLATFORM_VIBER},
    {"line.exe", MREC_PLATFORM_LINE},
    {"wechat.exe", MREC_PLATFORM_WECHAT},
    {"weixin.exe", MREC_PLATFORM_WECHAT},
    {"zalo.exe", MREC_PLATFORM_ZALO},
    {"jitsi meet.exe", MREC_PLATFORM_JITSI},
    {"element.exe", MREC_PLATFORM_ELEMENT},
    {"element nightly.exe", MREC_PLATFORM_ELEMENT},
    {"mattermost.exe", MREC_PLATFORM_MATTERMOST},
    {"nextcloud talk.exe", MREC_PLATFORM_NEXTCLOUD_TALK},
    {"jami.exe", MREC_PLATFORM_JAMI},
    {"wemeetapp.exe", MREC_PLATFORM_TENCENT_MEETING},
    {"wxwork.exe", MREC_PLATFORM_WECOM},
};

/*
 * Browsers whose tabs may host a call. They need both a call URL/title and
 * microphone activity.
 */
static const char *const kBrowserBundlePrefixes[] = {
    "com.google.chrome",       "org.chromium.chromium",      "com.apple.safari",
    "com.microsoft.edgemac",   "org.mozilla.firefox",        "company.thebrowser.browser",
    "com.brave.browser",       "com.vivaldi.vivaldi",        "com.operasoftware.opera",
    "ai.perplexity.comet",     "app.zen-browser.zen",        "company.thebrowser.dia",
    "com.operasoftware.operagx", "org.mozilla.firefoxdeveloperedition", "org.mozilla.nightly",
    "com.operasoftware.operanext", "com.operasoftware.operadeveloper",
    "com.apple.safaritechnologypreview",
    "net.imput.helium",        "com.operasoftware.operaair", "net.waterfox.waterfox",
    "com.kagi.kagimacos",      "com.naver.whale",            "ru.yandex.desktop.yandex-browser",
};

static const char *const kBrowserExecutables[] = {
    "chrome.exe",  "chromium.exe", "msedge.exe", "firefox.exe", "brave.exe",
    "arc.exe",     "vivaldi.exe",  "opera.exe",  "comet.exe",   "zen.exe",
    "browser.exe", "waterfox.exe",
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
    {"app.goto.com", "/meeting/", 0, MREC_PLATFORM_GOTO},
    {"meet.goto.com", "/", 0, MREC_PLATFORM_GOTO},
    {"global.gotomeeting.com", "/join/", 0, MREC_PLATFORM_GOTO},
    {"gotomeet.me", "/", 0, MREC_PLATFORM_GOTO},
    {"meetings.dialpad.com", "/room/", 0, MREC_PLATFORM_DIALPAD},
    {"v.ringcentral.com", "/join/", 0, MREC_PLATFORM_RINGCENTRAL},
    {"tuple.app", "/c/", 0, MREC_PLATFORM_TUPLE},
    {"telemost.yandex.ru", "/j/", 0, MREC_PLATFORM_TELEMOST},
    {"8x8.vc", "/vpaas-magic-cookie-", 0, MREC_PLATFORM_8X8},
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

/* GoTo's numeric call routes must not include account/settings pages. */
static int HasGoToMeetingID(const URL *url, const char *path) {
  const size_t prefix = strlen(path);
  const char *id = url->path + prefix;
  size_t length = url->path_length - prefix;
  if (length && id[length - 1] == '/') length--;
  size_t digits = 0;
  for (size_t i = 0; i < length; i++) {
    if (id[i] >= '0' && id[i] <= '9') digits++;
    else if (length != 11 || (i != 3 && i != 7) || id[i] != '-') return 0;
  }
  return digits == 9;
}

/* A JaaS call needs both the complete tenant ID and a room. */
static int HasJaaSRoom(const URL *url, const char *path) {
  const size_t prefix = strlen(path);
  if (url->path_length <= prefix + 33) return 0;
  for (size_t i = prefix; i < prefix + 32; i++) {
    const char c = Lower(url->path[i]);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return url->path[prefix + 32] == '/' && url->path[prefix + 33] != '/';
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
      if (pattern->platform == MREC_PLATFORM_GOTO && strcmp(pattern->host, "gotomeet.me") != 0 &&
          !HasGoToMeetingID(url, pattern->path)) continue;
      if (pattern->platform == MREC_PLATFORM_8X8 && !HasJaaSRoom(url, pattern->path)) continue;
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
    {MREC_PLATFORM_MEET, MREC_CONTROL_STATUS, NULL, "end the call or just leave?", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "join now", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "join now without camera", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "ask to join", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "rejoin", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "switch here", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_MEET, MREC_CONTROL_BUTTON, NULL, "switch here without camera", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "end meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "end meeting for all", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, "leave", NULL, MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_MENU_ITEM, NULL, "leave meeting", MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_MENU_ITEM, NULL, "end meeting", MREC_CALL_SIGNAL_JOINED},
    /* macOS Zoom keeps this in-meeting action when its toolbar is hidden. */
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_MENU_ITEM, "oncopyinvitelink:", NULL, MREC_CALL_SIGNAL_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "join meeting", MREC_CALL_SIGNAL_NOT_JOINED},
    {MREC_PLATFORM_ZOOM, MREC_CONTROL_BUTTON, NULL, "start", MREC_CALL_SIGNAL_NOT_JOINED},
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
    {MREC_PLATFORM_MEET, MREC_CONTROL_STATUS, NULL, "please wait until a meeting host brings you into the call", MREC_CALL_SIGNAL_BLOCKED},
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
    case MREC_PLATFORM_GOTO: return "goto";
    case MREC_PLATFORM_DIALPAD: return "dialpad";
    case MREC_PLATFORM_WHATSAPP: return "whatsapp";
    case MREC_PLATFORM_AIRCALL: return "aircall";
    case MREC_PLATFORM_RINGCENTRAL: return "ringcentral";
    case MREC_PLATFORM_8X8: return "8x8";
    case MREC_PLATFORM_VOOV: return "voov";
    case MREC_PLATFORM_TUPLE: return "tuple";
    case MREC_PLATFORM_GATHER: return "gather";
    case MREC_PLATFORM_LARK: return "lark";
    case MREC_PLATFORM_ROAM: return "roam";
    case MREC_PLATFORM_CLICKUP: return "clickup";
    case MREC_PLATFORM_TELEMOST: return "telemost";
    case MREC_PLATFORM_TELEGRAM: return "telegram";
    case MREC_PLATFORM_SIGNAL: return "signal";
    case MREC_PLATFORM_VIBER: return "viber";
    case MREC_PLATFORM_LINE: return "line";
    case MREC_PLATFORM_WECHAT: return "wechat";
    case MREC_PLATFORM_ZALO: return "zalo";
    case MREC_PLATFORM_JITSI: return "jitsi";
    case MREC_PLATFORM_ELEMENT: return "element";
    case MREC_PLATFORM_MATTERMOST: return "mattermost";
    case MREC_PLATFORM_NEXTCLOUD_TALK: return "nextcloud-talk";
    case MREC_PLATFORM_JAMI: return "jami";
    case MREC_PLATFORM_TENCENT_MEETING: return "tencent-meeting";
    case MREC_PLATFORM_WECOM: return "wecom";
    case MREC_PLATFORM_UNKNOWN: break;
  }
  return "unknown";
}
