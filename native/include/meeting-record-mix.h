#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mrec_pcm_mixer mrec_pcm_mixer;

/* Optional timestamp-aligned mono s16le output, independent of capture.
 * One owner must serialize calls; do not call from a realtime audio callback.
 * A source index identifies a separate track. Raw input is never modified.
 * Input rates must be finite and within 1..UINT32_MAX Hz. */
mrec_pcm_mixer *mrec_pcm_mixer_create(uint32_t sample_rate, uint32_t source_count);
void mrec_pcm_mixer_destroy(mrec_pcm_mixer *);
/* Returns 0 on success, -1 for invalid arguments, -2 on allocation failure. */
int32_t mrec_pcm_mixer_write(mrec_pcm_mixer *, uint32_t source,
    const float *samples, uint32_t frames, uint32_t channels, double sample_rate,
    uint64_t host_time_ns);
int32_t mrec_pcm_mixer_set_muted(mrec_pcm_mixer *, uint32_t source, int32_t muted);
/* Flush before pause/stop; restart excludes paused time and keeps unread PCM. */
int32_t mrec_pcm_mixer_finish_segment(mrec_pcm_mixer *);
int32_t mrec_pcm_mixer_restart(mrec_pcm_mixer *);
/* Returns 1 for a chunk, 0 when none is ready, or a negative error.
 * PCM belongs to the mixer until the next read/destroy. RMS uses encoded PCM. */
int32_t mrec_pcm_mixer_read(mrec_pcm_mixer *, const uint8_t **pcm, size_t *bytes, float *level);

#ifdef __cplusplus
}
#endif
