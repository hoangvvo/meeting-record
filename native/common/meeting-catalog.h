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

/* macOS: the conferencing app owning `bundle_id`. Prefixes match, so helpers count. */
int32_t mrec_catalog_bundle_platform(const char *bundle_id);

/* macOS: the bundle-id prefix of the browser owning `bundle_id`, or NULL. */
const char *mrec_catalog_bundle_browser(const char *bundle_id);

/* Windows: platform for a conferencing executable. */
int32_t mrec_catalog_executable_platform(const char *executable);

/* Windows: the browser executable `executable` names, or NULL. */
const char *mrec_catalog_executable_browser(const char *executable);

/* The meeting a URL points at. */
int32_t mrec_catalog_url_platform(const char *url);

/*
 * The meeting a window or tab title names. `page_length` receives how many bytes
 * of `title` are the page's own title, without the browser's suffixes.
 */
int32_t mrec_catalog_title_platform(const char *title, size_t *page_length);

/*
 * How likely audio activity is a meeting; 0 when it is not one. A browser needs
 * `recognised`, the platform its URL or title names, or a live microphone.
 */
int32_t mrec_catalog_confidence(int32_t is_browser, int32_t playing_audio,
                                int32_t using_mic, int32_t recognised);

#ifdef __cplusplus
}
#endif

#endif /* MEETING_RECORD_MEETING_CATALOG_H */
