/* Deterministic backend for the real N-API binding's lifecycle tests. */
#include "meeting-record.h"
#include "meeting-record-detect.h"
#include <napi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
struct Config {
  unsigned initial_packets = 40;
  unsigned tail_packets = 2;
  unsigned start_delay_ms = 0;
  int start_status = MREC_OK;
  int format_status = MREC_OK;
  int microphone_format_status = MREC_OK;
  int initial_event_type = -1;
};
Config config;
mrec_audio_callback audio;
void *audio_data;
mrec_audio_callback microphone_audio;
void *microphone_data;
mrec_track_event_callback track_event;
void *event_data;
std::atomic<bool> running{false};
std::atomic<unsigned> starts{0};
std::atomic<unsigned> stops{0};
uint32_t system_channels = 1;
int32_t microphone_source = 0;
uint32_t target_pid = 0;
bool system_wide = false;

void Packet(mrec_audio_callback callback, void *data, unsigned index,
            uint32_t channels, double rate = 48000, uint32_t frames = 2,
            bool silent = false) {
  if (!callback) return;
  std::vector<float> samples(frames * channels);
  for (size_t sample = 0; sample < samples.size(); sample++)
    samples[sample] = silent ? 0 : (sample % 2 ? -0.25f : 0.25f);
  callback(samples.data(), frames, channels, rate,
           1000000000ull + index * 10000000ull, data);
}

int Number(Napi::Object object, const char *name, int fallback) {
  return object.Has(name) ? object.Get(name).As<Napi::Number>().Int32Value() : fallback;
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
int32_t mrec_start_tracks_raw(const uint32_t *pids, size_t pid_count, int32_t global_mixdown,
    int32_t mono, int32_t, int32_t microphone, mrec_audio_callback callback, void *data,
    mrec_audio_callback microphone_callback, void *microphone_user_data,
    mrec_track_event_callback event_callback, void *event_user_data) {
  starts++;
  std::this_thread::sleep_for(std::chrono::milliseconds(config.start_delay_ms));
  if (config.start_status != MREC_OK) return config.start_status;
  audio = callback;
  audio_data = data;
  microphone_audio = microphone_callback;
  microphone_data = microphone_user_data;
  track_event = event_callback;
  event_data = event_user_data;
  system_channels = mono ? 1 : 2;
  microphone_source = microphone;
  target_pid = pid_count ? pids[0] : 0;
  system_wide = global_mixdown != 0;
  running = true;
  for (unsigned index = 0; index < config.initial_packets; index++) {
    Packet(audio, audio_data, index, system_channels);
    Packet(microphone_audio, microphone_data, index, 2, 44100);
  }
  if (config.initial_event_type >= 0) {
    mrec_track_event event{};
    event.track = MREC_TRACK_SYSTEM_AUDIO;
    event.type = static_cast<mrec_track_event_type>(config.initial_event_type);
    event.reason = MREC_TRACK_REASON_SERVICE_RESET;
    track_event(&event, event_data);
  }
  return MREC_OK;
}
mrec_status mrec_current_format(double *rate, uint32_t *channels) {
  *rate = 48000;
  *channels = system_channels;
  return static_cast<mrec_status>(config.format_status);
}
mrec_status mrec_current_microphone_format(double *rate, uint32_t *channels) {
  *rate = 44100;
  *channels = 2;
  return microphone_audio ? static_cast<mrec_status>(config.microphone_format_status) : MREC_ERR_NOT_RUNNING;
}
mrec_status mrec_current_microphone_name(char *name, size_t capacity) {
  if (!microphone_audio) return MREC_ERR_NOT_RUNNING;
  std::snprintf(name, capacity, "%s", "Fixture microphone");
  return MREC_OK;
}
int32_t mrec_is_running() { return running ? 1 : 0; }
const char *mrec_last_error() { return "fixture failure"; }
mrec_status mrec_stop() {
  if (!running.exchange(false)) return MREC_ERR_NOT_RUNNING;
  stops++;
  for (unsigned tail = 0; tail < config.tail_packets; tail++) {
    if (tail) std::this_thread::sleep_for(std::chrono::milliseconds(75));
    const unsigned index = config.initial_packets + tail;
    Packet(audio, audio_data, index, system_channels);
    Packet(microphone_audio, microphone_data, index, 2, 44100);
  }
  return MREC_OK;
}
}

void RegisterTestBackend(Napi::Env env, Napi::Object exports) {
  exports.Set("testConfigure", Napi::Function::New(env, [](const Napi::CallbackInfo &info) {
    Napi::Object options = info[0].As<Napi::Object>();
    config.initial_packets = Number(options, "initialPackets", 40);
    config.tail_packets = Number(options, "tailPackets", 2);
    config.start_delay_ms = Number(options, "startDelayMs", 0);
    config.start_status = Number(options, "startStatus", MREC_OK);
    config.format_status = Number(options, "formatStatus", MREC_OK);
    config.microphone_format_status = Number(options, "microphoneFormatStatus", MREC_OK);
    config.initial_event_type = Number(options, "initialEventType", -1);
    starts = 0;
    stops = 0;
  }));
  exports.Set("testAudio", Napi::Function::New(env, [](const Napi::CallbackInfo &info) {
    Napi::Object packet = info[0].As<Napi::Object>();
    const bool mic = packet.Has("track") && packet.Get("track").As<Napi::String>().Utf8Value() == "microphone";
    Packet(mic ? microphone_audio : audio, mic ? microphone_data : audio_data,
           Number(packet, "index", 100), Number(packet, "channels", mic ? 2 : system_channels),
           Number(packet, "sampleRate", mic ? 44100 : 48000), Number(packet, "frameCount", 2),
           packet.Has("silent") && packet.Get("silent").As<Napi::Boolean>().Value());
  }));
  exports.Set("testEvent", Napi::Function::New(env, [](const Napi::CallbackInfo &info) {
    Napi::Object event = info[0].As<Napi::Object>();
    mrec_track_event value{};
    const auto track = event.Get("track");
    value.track = track.IsNumber() ? static_cast<mrec_track>(track.As<Napi::Number>().Int32Value())
        : track.As<Napi::String>().Utf8Value() == "microphone"
            ? MREC_TRACK_MICROPHONE : MREC_TRACK_SYSTEM_AUDIO;
    value.type = static_cast<mrec_track_event_type>(Number(event, "type", 0));
    value.reason = static_cast<mrec_track_reason>(Number(event, "reason", 0));
    value.host_time_ns = 1000000000ull;
    if (event.Has("deviceName"))
      std::snprintf(value.device_name, sizeof value.device_name, "%s",
                    event.Get("deviceName").As<Napi::String>().Utf8Value().c_str());
    if (event.Has("message"))
      std::snprintf(value.message, sizeof value.message, "%s",
                    event.Get("message").As<Napi::String>().Utf8Value().c_str());
    track_event(&value, event_data);
  }));
  exports.Set("testStats", Napi::Function::New(env, [](const Napi::CallbackInfo &info) {
    Napi::Object stats = Napi::Object::New(info.Env());
    stats.Set("starts", starts.load());
    stats.Set("stops", stops.load());
    stats.Set("microphoneSource", microphone_source);
    stats.Set("pid", target_pid);
    stats.Set("systemWide", system_wide);
    return stats;
  }));
}
