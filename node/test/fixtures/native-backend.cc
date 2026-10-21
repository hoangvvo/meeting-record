/* Deterministic backend for the real N-API binding's lifecycle tests. */
#include "meeting-record.h"
#include "meeting-record-detect.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
mrec_audio_callback audio;
void *audio_data;
std::atomic<bool> running{false};
void Packet(unsigned index) {
  const float samples[] = {0.25f, -0.25f};
  audio(samples, 2, 1, 48000, 1000000000ull + index * 10000000ull, audio_data);
}
}
extern "C" {
mrec_permission mrec_audio_permission_status() { return MREC_PERM_GRANTED; }
mrec_permission mrec_microphone_permission_status() { return MREC_PERM_GRANTED; }
mrec_permission mrec_accessibility_permission_status() { return MREC_PERM_GRANTED; }
mrec_status mrec_request_audio_permission() { return MREC_OK; }
mrec_status mrec_request_microphone_permission() { return MREC_OK; }
mrec_status mrec_request_accessibility_permission() { return MREC_OK; }
mrec_status mrec_list_audio_processes(mrec_process *, size_t, size_t *count) { *count = 0; return MREC_ERR_INTERNAL; }
mrec_status mrec_list_audio_apps(mrec_process *, size_t, size_t *count) { *count = 0; return MREC_ERR_INTERNAL; }
mrec_status mrec_scan(mrec_meeting *, size_t, size_t *count) { *count = 0; return MREC_ERR_INTERNAL; }
mrec_status mrec_watch_start(mrec_meeting_callback, void *) { return MREC_OK; }
mrec_status mrec_watch_stop() { return MREC_OK; }
int32_t mrec_is_watching() { return 0; }
const char *mrec_platform_id(mrec_platform) { return "unknown"; }
int32_t mrec_start_tracks_raw(const uint32_t *, size_t, int32_t, int32_t, int32_t, int32_t,
    mrec_audio_callback callback, void *data, mrec_audio_callback, void *, mrec_track_event_callback, void *) {
  audio = callback;
  audio_data = data;
  running = true;
  for (unsigned index = 0; index < 40; index++) Packet(index);
  return MREC_OK;
}
mrec_status mrec_current_format(double *rate, uint32_t *channels) { *rate = 48000; *channels = 1; return MREC_OK; }
mrec_status mrec_current_microphone_format(double *rate, uint32_t *channels) { *rate = 48000; *channels = 1; return MREC_OK; }
mrec_status mrec_current_microphone_name(char *name, size_t) { *name = 0; return MREC_ERR_NOT_RUNNING; }
int32_t mrec_is_running() { return running ? 1 : 0; }
const char *mrec_last_error() { return "fixture failure"; }
mrec_status mrec_stop() {
  if (!running.exchange(false)) return MREC_ERR_NOT_RUNNING;
  Packet(40);
  std::this_thread::sleep_for(std::chrono::milliseconds(75));
  Packet(41);
  return MREC_OK;
}
}
