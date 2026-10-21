/* Shared source recovery and track events. Safe to import into Swift. */
#ifndef MEETING_RECORD_TRACKS_H
#define MEETING_RECORD_TRACKS_H

#include <stddef.h>
#include <stdint.h>

#include "../include/meeting-record-events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A track's sources are numbered from 0; this one stands for the whole track. */
#define MREC_TRACK_WHOLE 0xFFFFFFFFu

/* Starts a session and returns its identity. Old identities never publish into it. */
uint64_t mrec_tracks_open(mrec_track_event_callback callback, void *user_data);

/* Nothing is published once this returns. */
void mrec_tracks_close(void);

/* Configure a live track. Returns 0 for stale sessions or allocation failure. */
int32_t mrec_track_started(uint64_t session, mrec_track track, size_t source_count,
                           const char *device_name);

/* A source stopped: DEGRADED while others remain, INTERRUPTED when none remain. */
void mrec_track_interrupted(uint64_t session, mrec_track track, uint32_t source,
                            mrec_track_reason reason, const char *message);

/*
 * `source` delivers again, on `device_name` unless NULL: publishes DEVICE_CHANGED
 * for a new device, then RESUMED when every source is available, or DEGRADED
 * while some remain lost.
 */
void mrec_track_resumed(uint64_t session, mrec_track track, uint32_t source,
                        const char *device_name);

/* Reopens a source: 1 on success, after writing its device name if it has one. */
typedef int32_t (*mrec_track_attempt)(void *context, char *device_name, size_t capacity);

/* Sleeps `milliseconds`: 0 once the session is stopping. */
typedef int32_t (*mrec_track_wait)(void *context, uint32_t milliseconds);

/*
 * Reopens `source` right away, so a quick switch is a device change and not an
 * interruption. Otherwise interrupts the track and retries with backoff, at most
 * 2 s apart, until it reopens (1) or `wait` reports a stop (0).
 */
int32_t mrec_track_recover(uint64_t session, mrec_track track, uint32_t source,
                           mrec_track_reason reason,
                           const char *message, mrec_track_attempt attempt,
                           mrec_track_wait wait, void *context);

/* The wait before retry `failures`. */
uint32_t mrec_track_retry_delay_ms(uint32_t failures);

#ifdef __cplusplus
}
#endif

#endif /* MEETING_RECORD_TRACKS_H */
