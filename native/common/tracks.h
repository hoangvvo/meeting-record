/*
 * Source loss, shared by the macOS and Windows backends. A source that stops
 * interrupts its track and is retried until it returns; recording never stops
 * for it. Backends report what happened and how to reopen a source; this decides
 * what to publish, and when to retry.
 *
 * Internal; safe to import into Swift.
 */
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

/* Starts publishing to `callback`, which may be NULL, for a new session. */
void mrec_tracks_open(mrec_track_event_callback callback, void *user_data);

/* Nothing is published once this returns. */
void mrec_tracks_close(void);

/* A track that started live, on `device_name` when it is a microphone. */
void mrec_track_started(mrec_track track, const char *device_name);

/* `source` stopped: publishes INTERRUPTED when it is the track's first. */
void mrec_track_interrupted(mrec_track track, uint32_t source, mrec_track_reason reason,
                            const char *message);

/*
 * `source` delivers again, on `device_name` unless NULL: publishes DEVICE_CHANGED
 * for a new device, then RESUMED when it was the track's last stopped source.
 */
void mrec_track_resumed(mrec_track track, uint32_t source, const char *device_name);

/* Reopens a source: 1 on success, after writing its device name if it has one. */
typedef int32_t (*mrec_track_attempt)(void *context, char *device_name, size_t capacity);

/* Sleeps `milliseconds`: 0 once the session is stopping. */
typedef int32_t (*mrec_track_wait)(void *context, uint32_t milliseconds);

/*
 * Reopens `source` right away, so a quick switch is a device change and not an
 * interruption. Otherwise interrupts the track and retries with backoff, at most
 * 2 s apart, until it reopens (1) or `wait` reports a stop (0).
 */
int32_t mrec_track_recover(mrec_track track, uint32_t source, mrec_track_reason reason,
                           const char *message, mrec_track_attempt attempt,
                           mrec_track_wait wait, void *context);

/* The wait before retry `failures`. */
uint32_t mrec_track_retry_delay_ms(uint32_t failures);

#ifdef __cplusplus
}
#endif

#endif /* MEETING_RECORD_TRACKS_H */
