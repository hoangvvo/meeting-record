#pragma once

#include "meeting-record-detect.h"

#include <algorithm>
#include <cctype>
#include <string>

/*
 * Which processes are conferencing apps or browsers, and how to recognise a meeting
 * in a browser's URL or title. The Windows counterpart of MeetingCatalog.swift.
 */
namespace meeting_record {

struct NativeApp {
  const char *exe; /* lowercased executable name */
  mrec_platform platform;
};

/* Matched on executable name; Windows has no bundle identifiers. */
constexpr NativeApp kNativeApps[] = {
    {"zoom.exe", MREC_PLATFORM_ZOOM},
    {"cpthost.exe", MREC_PLATFORM_ZOOM}, /* Zoom's screen-share helper */
    {"teams.exe", MREC_PLATFORM_TEAMS},
    {"ms-teams.exe", MREC_PLATFORM_TEAMS},
    {"ms-teams_modulehost.exe", MREC_PLATFORM_TEAMS}, /* new Teams media host */
    {"msteamsvdi.exe", MREC_PLATFORM_TEAMS},
    {"msteams.exe", MREC_PLATFORM_TEAMS},
    {"webexmta.exe", MREC_PLATFORM_WEBEX},
    {"webex.exe", MREC_PLATFORM_WEBEX},
    {"atmgr.exe", MREC_PLATFORM_WEBEX},
    {"slack.exe", MREC_PLATFORM_SLACK},
    {"discord.exe", MREC_PLATFORM_DISCORD},
};

constexpr const char *kBrowsers[] = {"chrome.exe", "msedge.exe", "firefox.exe",
                                     "brave.exe",  "arc.exe",    "vivaldi.exe",
                                     "opera.exe"};

struct UrlPattern {
  const char *pattern;
  mrec_platform platform;
};

/* Must match MeetingCatalog.urlPatterns in the macOS backend. */
constexpr UrlPattern kUrlPatterns[] = {
    {"meet.google.com", MREC_PLATFORM_MEET},
    {"teams.microsoft.com", MREC_PLATFORM_TEAMS},
    {"teams.live.com", MREC_PLATFORM_TEAMS},
    {"zoom.us/j/", MREC_PLATFORM_ZOOM},
    {"zoom.us/wc/", MREC_PLATFORM_ZOOM},
    {"/webex.com/meet", MREC_PLATFORM_WEBEX},
    {"webex.com/wbxmjs", MREC_PLATFORM_WEBEX},
    {"app.slack.com/huddle", MREC_PLATFORM_SLACK},
    {"discord.com/channels", MREC_PLATFORM_DISCORD},
    {"whereby.com/", MREC_PLATFORM_GENERIC_BROWSER},
    {"meet.jit.si", MREC_PLATFORM_GENERIC_BROWSER},
    {"app.gather.town", MREC_PLATFORM_GENERIC_BROWSER},
    {"around.co/", MREC_PLATFORM_GENERIC_BROWSER},
};
inline std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
inline mrec_platform NativePlatformFor(const std::string &exe_lower) {
  for (const auto &app : kNativeApps) {
    if (exe_lower == app.exe) return app.platform;
  }
  return MREC_PLATFORM_UNKNOWN;
}

inline bool IsBrowser(const std::string &exe_lower) {
  for (const char *browser : kBrowsers) {
    if (exe_lower == browser) return true;
  }
  return false;
}

inline mrec_platform PlatformForText(const std::string &text) {
  if (text.empty()) return MREC_PLATFORM_UNKNOWN;
  const std::string haystack = ToLower(text);
  for (const auto &entry : kUrlPatterns) {
    if (haystack.find(entry.pattern) != std::string::npos) return entry.platform;
  }
  return MREC_PLATFORM_UNKNOWN;
}

/* "Meet - abc-defg-hij" without browser suffixes, or empty if `title` is not a Meet call. */
inline std::string MeetTitle(const std::string &title) {
  constexpr char kPrefix[] = "Meet - ";
  constexpr size_t kPrefixLength = sizeof kPrefix - 1;
  constexpr size_t kCodeLength = 12; /* abc-defg-hij */
  if (title.size() < kPrefixLength + kCodeLength ||
      title.compare(0, kPrefixLength, kPrefix) != 0) {
    return {};
  }
  for (size_t index = 0; index < kCodeLength; index++) {
    const char c = title[kPrefixLength + index];
    const bool separator = index == 3 || index == 8;
    if (separator ? c != '-' : (c < 'a' || c > 'z')) return {};
  }
  const size_t end = kPrefixLength + kCodeLength;
  if (end < title.size() && std::isalnum(static_cast<unsigned char>(title[end]))) {
    return {};
  }
  return title.substr(0, end);
}

/* Like PlatformForText, and narrows `title` to the page title when it can. */
inline mrec_platform PlatformForTitle(std::string &title) {
  if (std::string meet = MeetTitle(title); !meet.empty()) {
    title = std::move(meet);
    return MREC_PLATFORM_MEET;
  }
  return PlatformForText(title);
}

} // namespace meeting_record
