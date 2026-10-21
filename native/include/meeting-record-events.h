/*
 * Track events. A lost source never stops a recording: its track is interrupted
 * and retried until the device returns. Process exit requires selecting a new
 * target. Partial source loss is reported separately as degraded capture.
 */
#ifndef MEETING_RECORD_EVENTS_H
#define MEETING_RECORD_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MREC_TRACK_SYSTEM_AUDIO = 0,
  MREC_TRACK_MICROPHONE = 1,
} mrec_track;

typedef enum {
  /* No source is available. The session remains alive. */
  MREC_TRACK_INTERRUPTED = 0,
  MREC_TRACK_RESUMED = 1,
  /* The microphone moved to `device_name`. */
  MREC_TRACK_DEVICE_CHANGED = 2,
  /* Some sources are unavailable; the remaining sources still deliver audio. */
  MREC_TRACK_DEGRADED = 3,
} mrec_track_event_type;

/* Reason for source loss. */
typedef enum {
  MREC_TRACK_REASON_UNKNOWN = 0,
  MREC_TRACK_REASON_DEVICE_REMOVED = 1, /* unplugged, disabled, or none left */
  MREC_TRACK_REASON_DEVICE_CHANGED = 2, /* the device in use changed and did not reopen */
  MREC_TRACK_REASON_FORMAT_CHANGED = 3,
  MREC_TRACK_REASON_APP_EXITED = 4, /* every captured app exited */
  MREC_TRACK_REASON_SERVICE_RESET = 5, /* the audio service restarted */
} mrec_track_reason;

typedef struct {
  mrec_track track;
  mrec_track_event_type type;
  mrec_track_reason reason; /* INTERRUPTED or DEGRADED */
  uint64_t host_time_ns; /* the clock of the audio callbacks */
  char device_name[256]; /* the microphone in use; empty for system audio */
  char message[512]; /* detail for logs */
} mrec_track_event;

/*
 * Called on an internal thread, never the audio thread: one event at a time, and
 * never after mrec_stop() returns. Do not call into the library from it.
 */
typedef void (*mrec_track_event_callback)(const mrec_track_event *event, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* MEETING_RECORD_EVENTS_H */
