#ifndef MEETING_RECORD_CAPTURE_H
#define MEETING_RECORD_CAPTURE_H
#include <stdint.h>

/* Bits in global_mixdown. Existing 0/1 calls retain their behavior. */
#define MREC_CAPTURE_SYSTEM 1
/* Windows: permit whole-output capture for known incompatible process targets.
 * With SYSTEM, explicitly capture all output devices. No silence heuristics.
 * Other platforms retain their existing capture path. */
#define MREC_CAPTURE_ALLOW_SYSTEM_FALLBACK 2

typedef enum {
  MREC_CAPTURE_SCOPE_PROCESS = 0,
  MREC_CAPTURE_SCOPE_SYSTEM = 1,
} mrec_capture_scope;

typedef enum {
  MREC_CAPTURE_REQUESTED = 0,
  MREC_CAPTURE_COMPATIBILITY_FALLBACK = 1,
} mrec_capture_reason;

typedef struct {
  /** Scope requested by the caller's capture target. */
  mrec_capture_scope requested_scope;
  /** Scope actually recorded; broader only when explicitly permitted. */
  mrec_capture_scope actual_scope;
  /** REQUESTED unless a fallback capture path was selected. */
  mrec_capture_reason reason;
  /** Output-device volume/mute may affect samples; not the current mute state. */
  int32_t volume_dependent;
  /** Playback from the recorder's own process is included. */
  int32_t includes_recorder;
  /** Compatibility app identity, empty when no app triggered fallback. */
  char app_id[128];
} mrec_capture_info;

#ifdef __cplusplus
extern "C" {
#endif
/* Snapshot of the active capture path. NOT_RUNNING if no snapshot exists.
 * Does not infer mute state: volume_dependent means endpoint volume/mute MAY
 * affect the samples, depending on the audio driver. */
int32_t mrec_current_capture_info(mrec_capture_info *out);
#ifdef __cplusplus
}
#endif
#endif
