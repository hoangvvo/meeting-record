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
 * Detection layers four independent signals, weakest dependency last:
 *
 *   1. process identity   — is a known conferencing app running? (free, exact)
 *   2. audio/mic activity — is it actually doing audio IO? (free, exact,
 *                           language-independent; the single strongest signal
 *                           that a call is *live* rather than merely open)
 *   3. window title / URL — which meeting? (needs Accessibility permission)
 *   4. AX tree scraping   — participants, mute state (needs Accessibility, and
 *                           depends on the app's UI language)
 *
 * Layers 1-2 need no permission at all and are enough to decide "record now".
 * Layers 3-4 are enrichment only: they depend on a permission the user may refuse
 * and on the target app's UI language. Never gate recording on them.
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

  /*
   * 1 when confidence clears the library's threshold. Prefer this over comparing
   * `confidence` yourself, so the weights can change without breaking callers.
   */
  int32_t should_record;

  uint64_t detected_at_ns;
} mrec_meeting;

/*
 * Point-in-time scan. Cheap enough to call every second or two: layers 1-2 are
 * pure property reads. Returns meetings sorted by descending confidence.
 */
mrec_status mrec_scan(mrec_meeting *out, size_t capacity,
                                  size_t *out_count);

/*
 * Record a detected meeting.
 *
 * Equivalent to filling an mrec_config with the meeting's audio_pids, which is
 * what you want in nearly all cases. The pid list is re-resolved at start time
 * rather than trusting what detection saw, so it stays correct when an app moves
 * audio between helper processes.
 */
mrec_status mrec_start_meeting(const mrec_meeting *meeting, mrec_audio_callback cb,
                               void *user_data);

/* ---- event-driven watching -------------------------------------------- */

typedef enum {
  MREC_MEETING_STARTED = 0,
  MREC_MEETING_UPDATED = 1, /* title changed, mic toggled, ... */
  MREC_MEETING_ENDED = 2,
} mrec_event;

/*
 * Invoked on an internal serial queue, not a realtime thread — you may allocate
 * and call back into a managed runtime here. For ENDED, only `platform` and
 * `pid` are guaranteed meaningful.
 */
typedef void (*mrec_meeting_callback)(const mrec_meeting *meeting,
                                       mrec_event event,
                                       void *user_data);

/*
 * Watch for meetings starting and ending.
 *
 * Driven by app launch/terminate notifications and CoreAudio process-list
 * changes, with a low-frequency poll as a backstop, so it does not busy-wait.
 */
mrec_status mrec_watch_start(mrec_meeting_callback cb,
                                         void *user_data);
mrec_status mrec_watch_stop(void);
int32_t mrec_is_watching(void);

/* ---- Accessibility permission (macOS) --------------------------------- */
/*
 * Only needed for titles, URLs and participants — never for deciding whether to
 * record. Unlike audio capture this one *does* have a real preflight API
 * (AXIsProcessTrusted), so it is cheap and honest.
 *
 * On Windows, UI Automation needs no grant: returns NOT_REQUIRED.
 */
mrec_permission mrec_accessibility_permission_status(void);

/*
 * Opens System Settings at the Accessibility pane. There is no in-app prompt for
 * this permission that grants without a trip to Settings, and the app must be
 * relaunched afterwards before the grant takes effect.
 */
mrec_status mrec_request_accessibility_permission(void);

const char *mrec_platform_name(mrec_platform platform);

#ifdef __cplusplus
}
#endif
#endif /* MEETING_RECORD_DETECT_H */
