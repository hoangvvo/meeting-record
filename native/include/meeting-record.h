/*
 * meeting-record — system-audio and microphone capture for macOS and Windows.
 *
 * One C ABI, two backends:
 *   macOS   : CoreAudio process taps (CATapDescription + private aggregate device)
 *   Windows : WASAPI process loopback (ActivateAudioInterfaceAsync)
 *
 * Both backends deliver interleaved float32 PCM on a realtime audio thread. The
 * callback must not allocate, lock, or call into a managed runtime.
 */
#ifndef MEETING_RECORD_H
#define MEETING_RECORD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MREC_OK = 0,
  MREC_ERR_UNSUPPORTED_OS = -1, /* macOS < 14.2 / Windows < 10 20348 */
  MREC_ERR_PERMISSION = -2,     /* TCC audio-capture not granted */
  MREC_ERR_ALREADY_RUNNING = -3,
  MREC_ERR_NOT_RUNNING = -4,
  MREC_ERR_NO_PROCESSES = -5, /* no target produced audio */
  MREC_ERR_TAP_FAILED = -6,
  MREC_ERR_DEVICE_FAILED = -7,
  MREC_ERR_IOPROC_FAILED = -8,
  MREC_ERR_INTERNAL = -9,
  MREC_ERR_BUFFER_TOO_SMALL = -10,
} mrec_status;

typedef enum {
  MREC_PERM_UNKNOWN = 0,
  MREC_PERM_GRANTED = 1,
  MREC_PERM_DENIED = 2,
  MREC_PERM_NOT_REQUIRED = 3, /* Windows: loopback needs no TCC equivalent */
} mrec_permission;

/*
 * macOS: whether this app holds kTCCServiceAudioCapture.
 *
 * No preflight API exists, so this probes with a throwaway tap and aggregate
 * device. Around 10ms when granted; the result is cached internally.
 */
mrec_permission mrec_audio_permission_status(void);

/*
 * macOS: trigger the one-time system prompt.
 *
 * Requirements learned the hard way (see README "Permission model"):
 *   - the host bundle needs NSAudioCaptureUsageDescription
 *   - the calling process must be a GUI app (NSApplication running) or TCC has
 *     nowhere to draw the prompt and the CoreAudio call blocks indefinitely
 *   - must NOT be called from the main thread, for the same reason
 *
 * Returns immediately; poll mrec_audio_permission_status() for the outcome.
 */
mrec_status mrec_request_audio_permission(void);

/* macOS microphone permission. Windows currently reports NOT_REQUIRED. */
mrec_permission mrec_microphone_permission_status(void);
mrec_status mrec_request_microphone_permission(void);

/*
 * A process the OS knows is doing audio IO. `pid` is what you pass to
 * mrec_start(); `bundle_id` may be empty for helper processes.
 */
typedef struct {
  uint32_t pid;
  int32_t is_running_output; /* 1 = currently rendering audio */
  int32_t is_running_input;  /* 1 = currently capturing audio */
  char bundle_id[256];
  char name[256];
} mrec_process;

/* Fills up to `capacity` entries and writes the count to `out_count`. */
mrec_status mrec_list_audio_processes(mrec_process *out, size_t capacity,
                                          size_t *out_count);

/*
 * Called on a realtime audio thread. `frames` is interleaved float32, valid only
 * for the duration of the call.
 */
typedef void (*mrec_audio_callback)(const float *frames, uint32_t frame_count,
                                     uint32_t channels, double sample_rate,
                                     uint64_t host_time_ns, void *user_data);

typedef struct {
  /*
   * Processes to capture. A process tap reads before the hardware mix, so it keeps
   * working while the user's output is muted.
   */
  const uint32_t *pids;
  size_t pid_count;

  /*
   * Capture the whole system mix instead. Yields silence while output is muted.
   * Ignored when pid_count > 0.
   */
  int32_t global_mixdown;

  /* 1 = mono mixdown, 0 = stereo. */
  int32_t mono;

  /* 1 = also mute the captured processes' output to the speakers. */
  int32_t mute_captured_output;
} mrec_config;

/* Exported symbol behind mrec_start(); call that instead. */
int32_t mrec_start_raw(const uint32_t *pids, size_t pid_count,
                         int32_t global_mixdown, int32_t mono,
                         int32_t mute_captured_output, mrec_audio_callback cb,
                         void *user_data);

typedef enum {
  MREC_MICROPHONE_NONE = 0,
  MREC_MICROPHONE_DEFAULT = 1,
} mrec_microphone_source;

/*
 * Extended entry point used by the Rust and Node bindings. System audio and the
 * microphone remain separate tracks and may negotiate different formats.
 */
int32_t mrec_start_tracks_raw(
    const uint32_t *pids, size_t pid_count, int32_t global_mixdown, int32_t mono,
    int32_t mute_captured_output, int32_t microphone,
    mrec_audio_callback system_audio_cb, void *system_audio_user_data,
    mrec_audio_callback microphone_cb, void *microphone_user_data);

static inline void mrec_config_defaults(mrec_config *cfg) {
  if (!cfg) return;
  cfg->pids = NULL;
  cfg->pid_count = 0;
  cfg->global_mixdown = 0;
  cfg->mono = 1;
  cfg->mute_captured_output = 0;
}

static inline mrec_status mrec_start(const mrec_config *cfg,
                                         mrec_audio_callback cb,
                                         void *user_data) {
  mrec_config local;
  if (!cfg) {
    mrec_config_defaults(&local);
    cfg = &local;
  }
  return (mrec_status)mrec_start_raw(cfg->pids, cfg->pid_count,
                                         cfg->global_mixdown, cfg->mono,
                                         cfg->mute_captured_output, cb, user_data);
}
mrec_status mrec_stop(void);
int32_t mrec_is_running(void);

/* Actual negotiated format, valid once running. */
mrec_status mrec_current_format(double *sample_rate, uint32_t *channels);
mrec_status mrec_current_microphone_format(double *sample_rate,
                                               uint32_t *channels);

/* Human-readable detail for the last failure. Never NULL. */
const char *mrec_last_error(void);

#ifdef __cplusplus
}
#endif
#endif /* MEETING_RECORD_H */
