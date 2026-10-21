#include "tracks.h"
#include "meeting-record.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <mach/mach_time.h>
#include <pthread.h>
#endif

enum {
  kTrackCount = 2,
  kFirstRetryDelayMs = 100,
  kMaxRetryDelayMs = 2000,
};

typedef struct {
  /* One bit per stopped source; the top bit is the whole track. */
  uint64_t stopped;
  char device_name[256];
} Track;

static Track g_tracks[kTrackCount];
static int g_open;
static mrec_track_event_callback g_callback;
static void *g_user_data;

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static void Lock(void) { AcquireSRWLockExclusive(&g_lock); }
static void Unlock(void) { ReleaseSRWLockExclusive(&g_lock); }

/* The clock the Windows backend stamps audio with. */
static uint64_t HostTimeNs(void) {
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
      !QueryPerformanceCounter(&counter)) {
    return GetTickCount64() * 1000000ull;
  }
  const uint64_t whole = (uint64_t)(counter.QuadPart / frequency.QuadPart);
  const uint64_t remainder = (uint64_t)(counter.QuadPart % frequency.QuadPart);
  return whole * 1000000000ull + remainder * 1000000000ull / (uint64_t)frequency.QuadPart;
}
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void Lock(void) { pthread_mutex_lock(&g_lock); }
static void Unlock(void) { pthread_mutex_unlock(&g_lock); }

/* The clock the macOS backend stamps audio with. */
static uint64_t HostTimeNs(void) {
  mach_timebase_info_data_t info;
  const uint64_t ticks = mach_absolute_time();
  if (mach_timebase_info(&info) != KERN_SUCCESS || info.denom == 0) return ticks;
  return ticks / info.denom * info.numer + ticks % info.denom * info.numer / info.denom;
}
#endif

static uint64_t SourceBit(uint32_t source) {
  if (source == MREC_TRACK_WHOLE) return 1ull << 63;
  return 1ull << (source < 63 ? source : 62);
}

static int IsTrack(mrec_track track) { return (int)track >= 0 && (int)track < kTrackCount; }

/* Under the lock, so events arrive one at a time and never after close. */
static void Publish(mrec_track track, mrec_track_event_type type, mrec_track_reason reason,
                    const char *message) {
  if (!g_callback) return;
  mrec_track_event event;
  memset(&event, 0, sizeof event);
  event.track = track;
  event.type = type;
  event.reason = reason;
  event.host_time_ns = HostTimeNs();
  snprintf(event.device_name, sizeof event.device_name, "%s", g_tracks[track].device_name);
  if (message) snprintf(event.message, sizeof event.message, "%s", message);
  g_callback(&event, g_user_data);
}

void mrec_tracks_open(mrec_track_event_callback callback, void *user_data) {
  Lock();
  memset(g_tracks, 0, sizeof g_tracks);
  g_open = 1;
  g_callback = callback;
  g_user_data = user_data;
  Unlock();
}

void mrec_tracks_close(void) {
  Lock();
  memset(g_tracks, 0, sizeof g_tracks);
  g_open = 0;
  g_callback = NULL;
  g_user_data = NULL;
  Unlock();
}

void mrec_track_started(mrec_track track, const char *device_name) {
  if (!IsTrack(track)) return;
  Lock();
  g_tracks[track].stopped = 0;
  snprintf(g_tracks[track].device_name, sizeof g_tracks[track].device_name, "%s",
           device_name ? device_name : "");
  Unlock();
}

void mrec_track_interrupted(mrec_track track, uint32_t source, mrec_track_reason reason,
                            const char *message) {
  if (!IsTrack(track)) return;
  Lock();
  if (g_open) {
    const uint64_t stopped = g_tracks[track].stopped;
    g_tracks[track].stopped |= SourceBit(source);
    if (stopped == 0) Publish(track, MREC_TRACK_INTERRUPTED, reason, message);
  }
  Unlock();
}

void mrec_track_resumed(mrec_track track, uint32_t source, const char *device_name) {
  if (!IsTrack(track)) return;
  Lock();
  if (g_open) {
    Track *state = &g_tracks[track];
    if (device_name && *device_name && strcmp(device_name, state->device_name) != 0) {
      snprintf(state->device_name, sizeof state->device_name, "%s", device_name);
      Publish(track, MREC_TRACK_DEVICE_CHANGED, MREC_TRACK_REASON_UNKNOWN, NULL);
    }
    const uint64_t stopped = state->stopped;
    state->stopped &= ~SourceBit(source);
    if (stopped != 0 && state->stopped == 0) {
      Publish(track, MREC_TRACK_RESUMED, MREC_TRACK_REASON_UNKNOWN, NULL);
    }
  }
  Unlock();
}

uint32_t mrec_track_retry_delay_ms(uint32_t failures) {
  const uint32_t limit = kMaxRetryDelayMs;
  if (failures >= 5) return limit;
  const uint32_t delay = (uint32_t)kFirstRetryDelayMs << failures;
  return delay < limit ? delay : limit;
}

int32_t mrec_track_recover(mrec_track track, uint32_t source, mrec_track_reason reason,
                           const char *message, mrec_track_attempt attempt,
                           mrec_track_wait wait, void *context) {
  char device_name[256] = "";
  if (attempt(context, device_name, sizeof device_name)) {
    mrec_track_resumed(track, source, device_name);
    return 1;
  }
  mrec_track_interrupted(track, source, reason, message);
  for (uint32_t failures = 0;; failures++) {
    if (!wait(context, mrec_track_retry_delay_ms(failures))) return 0;
    device_name[0] = '\0';
    if (attempt(context, device_name, sizeof device_name)) {
      mrec_track_resumed(track, source, device_name);
      return 1;
    }
  }
}

mrec_status mrec_current_microphone_name(char *name, size_t capacity) {
  if (!name || capacity == 0) return MREC_ERR_INTERNAL;
  Lock();
  const Track *microphone = &g_tracks[MREC_TRACK_MICROPHONE];
  const int known = g_open && microphone->device_name[0] != '\0';
  snprintf(name, capacity, "%s", known ? microphone->device_name : "");
  Unlock();
  return known ? MREC_OK : MREC_ERR_NOT_RUNNING;
}
