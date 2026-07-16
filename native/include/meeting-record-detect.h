/*
 * mrec_meeting — detect when the user is in a meeting, and which processes to
 * record.
 *
 * This is the companion module to meeting-record.h. The seam between them is the pid
 * list: detection tells you *what* to capture, mrec_start() captures it.
 *
 *     mrec_meeting m[8]; size_t n;
 *     mrec_scan(m, 8, &n);
 *     if (n > 0 && m[0].confidence >= 70) {
 *       mrec_config cfg; mrec_config_defaults(&cfg);
 *       cfg.pids = m[0].audio_pids;
 *       cfg.pid_count = m[0].audio_pid_count;
 *       mrec_start(&cfg, on_audio, NULL);
 *     }
 *
 * Four signals, in order of dependency:
 *
 *   1. process identity   — is a known conferencing app running?
 *   2. audio/mic activity — is it doing audio IO? Distinguishes a live call from
 *                           an app that is merely open.
 *   3. window title / URL — needs the Accessibility permission.
 *   4. AX tree scraping   — participants and mute state; also language-dependent.
 *
 * Layers 1-2 need no permission and are sufficient to decide whether to record.
 * Recording is never gated on 3-4.
 */
#ifndef MEETING_RECORD_DETECT_H
#define MEETING_RECORD_DETECT_H

#include <stddef.h>
#include <stdint.h>

#include "meeting-record.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MREC_PLATFORM_UNKNOWN = 0,
  MREC_PLATFORM_ZOOM,
  MREC_PLATFORM_TEAMS,
  MREC_PLATFORM_MEET,
  MREC_PLATFORM_WEBEX,
  MREC_PLATFORM_SLACK,
  MREC_PLATFORM_DISCORD,
  MREC_PLATFORM_GENERIC_BROWSER, /* a tab on a call URL we recognise */
} mrec_platform;

#define MREC_MAX_AUDIO_PIDS 16

typedef struct {
  mrec_platform platform;

  /* Main application process (the one the user sees). */
  uint32_t pid;

  /*
   * Processes actually doing audio IO for this meeting — pass these straight to
   * mrec_start(). Often *not* `pid`: Chrome, Electron and Teams render audio
   * from helper processes.
   */
  uint32_t audio_pids[MREC_MAX_AUDIO_PIDS];
  size_t audio_pid_count;

  char app_name[128];
  char title[512]; /* window or tab title; empty without Accessibility */
  char url[1024];  /* meeting URL for browser-based calls; may be empty */

  int32_t is_using_mic;     /* the user is (or could be) speaking */
  int32_t is_playing_audio; /* someone else is speaking */

  /*
   * 0-100: known app running 40, doing audio output +30, using the mic +25,
   * recognised meeting URL/title +5.
   */
  int32_t confidence;

  /* 1 when confidence clears the library's threshold. Prefer this to comparing
   * `confidence`, which lets the weights change without breaking callers. */
  int32_t should_record;

  uint64_t detected_at_ns;
} mrec_meeting;

/* Point-in-time scan, sorted by descending confidence. */
mrec_status mrec_scan(mrec_meeting *out, size_t capacity,
                                  size_t *out_count);

/*
 * Record a detected meeting. Equivalent to filling an mrec_config with its
 * audio_pids, except the pid list is re-resolved at start time, so it stays
 * correct when an app moves audio between helper processes.
 */
mrec_status mrec_start_meeting(const mrec_meeting *meeting, mrec_audio_callback cb,
                               void *user_data);

typedef enum {
  MREC_MEETING_STARTED = 0,
  MREC_MEETING_UPDATED = 1, /* title changed, mic toggled, ... */
  MREC_MEETING_ENDED = 2,
} mrec_event;

/*
 * Invoked on an internal serial queue, not a realtime thread, so allocation is
 * allowed. For ENDED, only `platform` and `pid` are meaningful.
 */
typedef void (*mrec_meeting_callback)(const mrec_meeting *meeting,
                                       mrec_event event,
                                       void *user_data);

/*
 * Watch for meetings starting and ending. Driven by app lifecycle notifications
 * and CoreAudio process-list changes, with a low-frequency poll as a backstop.
 */
mrec_status mrec_watch_start(mrec_meeting_callback cb,
                                         void *user_data);
mrec_status mrec_watch_stop(void);
int32_t mrec_is_watching(void);

/*
 * Needed only for titles, URLs and participants, never for deciding whether to
 * record. Backed by AXIsProcessTrusted, so this is a real preflight check.
 *
 * On Windows returns NOT_REQUIRED.
 */
mrec_permission mrec_accessibility_permission_status(void);

/*
 * Opens System Settings at the Accessibility pane; no in-app prompt exists. The
 * app must be relaunched before the grant takes effect.
 */
mrec_status mrec_request_accessibility_permission(void);

/*
 * Stable identifier: "zoom", "teams", "meet", "webex", "slack", "discord",
 * "browser", "unknown". The values never change. No human-readable labels are
 * provided; presentation and localisation are the caller's concern.
 *
 * Returns static storage owned by the library; do not free.
 */
const char *mrec_platform_id(mrec_platform platform);

#ifdef __cplusplus
}
#endif
#endif /* MEETING_RECORD_DETECT_H */
