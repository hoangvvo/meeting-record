/* Shared meeting classification. Integer platform values keep this Swift-compatible. */
#ifndef MEETING_RECORD_MEETING_CATALOG_H
#define MEETING_RECORD_MEETING_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Confidence at which a meeting should be recorded. */
#define MREC_CATALOG_RECORD_THRESHOLD 70

/* Watchers start a meeting once seen this long, so notification sounds never do. */
#define MREC_CATALOG_START_DELAY_MS 2000
/* Watchers end a meeting once missing this long, so one missed scan never does. */
#define MREC_CATALOG_END_DELAY_MS 3000

/* macOS: the conferencing app owning `bundle_id`. Dotted helper suffixes count. */
int32_t mrec_catalog_bundle_platform(const char *bundle_id);

/* macOS: the bundle-id prefix of the browser owning `bundle_id`, or NULL. */
const char *mrec_catalog_bundle_browser(const char *bundle_id);

/* Windows: platform for a conferencing executable. */
int32_t mrec_catalog_executable_platform(const char *executable);

/* Windows: the browser executable `executable` names, or NULL. */
const char *mrec_catalog_executable_browser(const char *executable);

/* Linux executable basenames; case-sensitive, independent of Windows names. */
int32_t mrec_catalog_linux_platform(const char *executable);
const char *mrec_catalog_linux_browser(const char *executable);

/* The call route a URL points at; home/chat URLs and embedded hosts do not count. */
int32_t mrec_catalog_url_platform(const char *url);

/* Service identity for scoped UI inspection, including Teams' general /v2/ app. */
int32_t mrec_catalog_context_platform(const char *url, const char *title);

/* Internal evidence. Consumers continue to use confidence/should_record. */
enum {
  MREC_CALL_UNKNOWN = 0,
  MREC_CALL_JOINED = 1,
  MREC_CALL_NOT_JOINED = 2,
};
enum {
  MREC_CONTROL_BUTTON = 1,
  MREC_CONTROL_MENU_ITEM = 2,
  MREC_CONTROL_TEXT = 3,
  MREC_CONTROL_STATUS = 4,
};
enum {
  MREC_CALL_SIGNAL_JOINED = 1,
  MREC_CALL_SIGNAL_NOT_JOINED = 2,
  MREC_CALL_SIGNAL_DISCONNECT = 4,
  MREC_CALL_SIGNAL_VOICE_CONNECTED = 8,
  MREC_CALL_SIGNAL_BLOCKED = 16,
};

/* Same identifier/label rules for AX on macOS and UI Automation on Windows. */
uint32_t mrec_catalog_call_control(int32_t platform, int32_t role,
                                 const char *identifier, const char *label);
int32_t mrec_catalog_call_state(uint32_t signals);

/*
 * The meeting a window or tab title names. `page_length` receives how many bytes
 * of `title` are the page's own title, without the browser's suffixes.
 */
int32_t mrec_catalog_title_platform(const char *title, size_t *page_length);

typedef struct {
  int32_t platform;
  size_t title_length;
  int32_t tab_index;
} mrec_browser_match;

/*
 * Prefer the active URL, then the window title, then a recognized tab title.
 * `tab_index` is -1 for the window, or a tab index whose URL must be omitted.
 * `title_length` is the selected page title's UTF-8 byte length.
 */
mrec_browser_match mrec_catalog_browser_match(const char *url, const char *title,
                                             const char *const *tab_titles,
                                             size_t tab_count);

/*
 * Conservative fallback when call UI is unknown. All clients need a live mic;
 * a browser also needs `recognised`, the call its URL or title names.
 */
int32_t mrec_catalog_confidence(int32_t is_browser, int32_t playing_audio,
                                int32_t using_mic, int32_t recognised);

/* Confirmed call UI supersedes the audio heuristic; pre-join/ended UI vetoes it. */
int32_t mrec_catalog_call_confidence(int32_t is_browser, int32_t playing_audio,
                                   int32_t using_mic, int32_t recognised,
                                   int32_t call_state);

#ifdef __cplusplus
}
#endif

#endif /* MEETING_RECORD_MEETING_CATALOG_H */
