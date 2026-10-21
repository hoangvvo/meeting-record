#include "tracks.h"
#include "meeting-record.h"

#include <stdio.h>
#include <stdlib.h>
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
  unsigned char *stopped;
  size_t source_count;
  size_t stopped_count;
  int whole_stopped;
  char device_name[256];
} Track;

static Track g_tracks[kTrackCount];
static int g_open;
static uint64_t g_session;
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

static int IsSession(uint64_t session) { return g_open && session == g_session; }

/* 0 = live, 1 = degraded, 2 = interrupted. Caller holds the lock. */
static int Availability(const Track *track) {
  if (track->whole_stopped || track->stopped_count == track->source_count) return 2;
  return track->stopped_count ? 1 : 0;
}

static void ClearTracks(void) {
  for (int index = 0; index < kTrackCount; index++) free(g_tracks[index].stopped);
  memset(g_tracks, 0, sizeof g_tracks);
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

uint64_t mrec_tracks_open(mrec_track_event_callback callback, void *user_data) {
  Lock();
  ClearTracks();
  if (++g_session == 0) ++g_session;
  g_open = 1;
  g_callback = callback;
  g_user_data = user_data;
  const uint64_t session = g_session;
  Unlock();
  return session;
}

void mrec_tracks_close(void) {
  Lock();
  ClearTracks();
  g_open = 0;
  g_callback = NULL;
  g_user_data = NULL;
  Unlock();
}

int32_t mrec_track_started(uint64_t session, mrec_track track, size_t source_count,
                           const char *device_name) {
  if (!IsTrack(track) || source_count == 0) return 0;
  unsigned char *sources = (unsigned char *)calloc(source_count, 1);
  if (!sources) return 0;
  Lock();
  if (!IsSession(session)) {
    Unlock();
    free(sources);
    return 0;
  }
  Track *state = &g_tracks[track];
  free(state->stopped);
  memset(state, 0, sizeof *state);
  state->stopped = sources;
  state->source_count = source_count;
  snprintf(state->device_name, sizeof state->device_name, "%s", device_name ? device_name : "");
  Unlock();
  return 1;
}

void mrec_track_interrupted(uint64_t session, mrec_track track, uint32_t source,
                            mrec_track_reason reason, const char *message) {
  if (!IsTrack(track)) return;
  Lock();
  Track *state = &g_tracks[track];
  if (IsSession(session) && state->source_count &&
      (source == MREC_TRACK_WHOLE || source < state->source_count)) {
    const int previous = Availability(state);
    const int whole_stopped = state->whole_stopped;
    if (source == MREC_TRACK_WHOLE) {
      state->whole_stopped = 1;
    } else if (source < state->source_count && !state->stopped[source]) {
      state->stopped[source] = 1;
      state->stopped_count++;
    }
    const int current = Availability(state);
    if (current != previous || state->whole_stopped != whole_stopped) {
      Publish(track, current == 2 ? MREC_TRACK_INTERRUPTED : MREC_TRACK_DEGRADED,
              reason, message);
    }
  }
  Unlock();
}

void mrec_track_resumed(uint64_t session, mrec_track track, uint32_t source,
                        const char *device_name) {
  if (!IsTrack(track)) return;
  Lock();
  Track *state = &g_tracks[track];
  if (IsSession(session) && state->source_count &&
      (source == MREC_TRACK_WHOLE || (source < state->source_count && !state->whole_stopped))) {
    if (device_name && *device_name &&
        strncmp(device_name, state->device_name, sizeof state->device_name - 1) != 0) {
      snprintf(state->device_name, sizeof state->device_name, "%s", device_name);
      Publish(track, MREC_TRACK_DEVICE_CHANGED, MREC_TRACK_REASON_UNKNOWN, NULL);
    }
    const int previous = Availability(state);
    if (source == MREC_TRACK_WHOLE) {
      state->whole_stopped = 0;
    } else if (source < state->source_count && state->stopped[source]) {
      state->stopped[source] = 0;
      state->stopped_count--;
    }
    const int current = Availability(state);
    if (current != previous) {
      Publish(track, current == 0 ? MREC_TRACK_RESUMED : MREC_TRACK_DEGRADED,
              MREC_TRACK_REASON_UNKNOWN, NULL);
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

static int CanRecover(uint64_t session, mrec_track track, uint32_t source) {
  if (!IsTrack(track)) return 0;
  Lock();
  const Track *state = &g_tracks[track];
  const int active = IsSession(session) && source < state->source_count && !state->whole_stopped;
  Unlock();
  return active;
}

int32_t mrec_track_recover(uint64_t session, mrec_track track, uint32_t source,
                           mrec_track_reason reason,
                           const char *message, mrec_track_attempt attempt,
                           mrec_track_wait wait, void *context) {
  if (!attempt || !wait) return 0;
  for (uint32_t failures = 0; CanRecover(session, track, source);) {
    char device_name[256] = "";
    const int reopened = attempt(context, device_name, sizeof device_name);
    device_name[sizeof device_name - 1] = '\0';
    if (!CanRecover(session, track, source)) return 0;
    if (reopened) {
      mrec_track_resumed(session, track, source, device_name);
      return 1;
    }
    if (failures == 0) mrec_track_interrupted(session, track, source, reason, message);
    if (!wait(context, mrec_track_retry_delay_ms(failures))) return 0;
    if (failures < 5) failures++;
  }
  return 0;
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
