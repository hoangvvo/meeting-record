#include "tracks.h"
#include "meeting-record.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static mrec_track_event g_events[16];
static int g_count;
static uint64_t g_session;

static void Record(const mrec_track_event *event, void *user_data) {
  (void)user_data;
  if (g_count < 16) g_events[g_count++] = *event;
}

static void Reset(void) {
  g_count = 0;
  g_session = mrec_tracks_open(Record, NULL);
  assert(mrec_track_started(g_session, MREC_TRACK_SYSTEM_AUDIO, 2, NULL));
  assert(mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, NULL));
}

static int Is(int index, mrec_track track, mrec_track_event_type type) {
  return index < g_count && g_events[index].track == track && g_events[index].type == type;
}

/*
 * A source that reopens on attempt `succeed_on`. A failed attempt may come with a
 * device notification (`wake`), and attempt `close_on` stops the session.
 */
typedef struct {
  int attempts;
  int succeed_on;
  int wake;
  int close_on;
  const char *device_name;
} Source;

static int32_t Attempt(void *context, char *device_name, size_t capacity) {
  Source *source = (Source *)context;
  if (++source->attempts == source->close_on) {
    mrec_tracks_close();
    return 0;
  }
  if (source->attempts < source->succeed_on) {
    if (source->wake) mrec_tracks_wake(g_session);
    return 0;
  }
  if (source->device_name) snprintf(device_name, capacity, "%s", source->device_name);
  return 1;
}

static double Seconds(void) {
  struct timespec now;
#ifdef __linux__
  clock_gettime(CLOCK_MONOTONIC, &now);
#else
  timespec_get(&now, TIME_UTC);
#endif
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int32_t ReplaceSession(void *context, char *device_name, size_t capacity) {
  Reset();
  snprintf(device_name, capacity, "%s", "Old microphone");
  return *(int *)context;
}

static int32_t UnterminatedName(void *context, char *device_name, size_t capacity) {
  (void)context;
  memset(device_name, 'b', capacity);
  return 1;
}

static void InvalidSourcesDoNotReopen(void) {
  Reset();
  Source source = {0, 1, 0, 0, "Wrong device"};
  assert(!mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 1,
                             MREC_TRACK_REASON_UNKNOWN, NULL, Attempt, &source));
  assert(source.attempts == 0 && g_count == 0);
  assert(!mrec_track_recover(g_session, (mrec_track)99, 0,
                             MREC_TRACK_REASON_UNKNOWN, NULL, Attempt, &source));
  assert(!mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0,
                             MREC_TRACK_REASON_UNKNOWN, NULL, NULL, &source));
  assert(!mrec_track_add_source(g_session, (mrec_track)99));
  assert(source.attempts == 0);
  mrec_track_resumed(g_session, MREC_TRACK_MICROPHONE, 1, "Wrong device");
  assert(g_count == 0);
  char name[16] = "untouched";
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_ERR_NOT_RUNNING);
  assert(name[0] == '\0');
  mrec_tracks_close();
}

static void DuplicateTransitionsAndNamesAreQuiet(void) {
  Reset();
  char long_name[512];
  memset(long_name, 'a', sizeof long_name - 1);
  long_name[sizeof long_name - 1] = '\0';
  mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, long_name);
  mrec_track_resumed(g_session, MREC_TRACK_MICROPHONE, 0, long_name);
  assert(g_count == 0);
  mrec_track_interrupted(g_session, MREC_TRACK_MICROPHONE, 0,
                         MREC_TRACK_REASON_DEVICE_REMOVED, long_name);
  mrec_track_interrupted(g_session, MREC_TRACK_MICROPHONE, 0,
                         MREC_TRACK_REASON_DEVICE_REMOVED, "duplicate");
  assert(g_count == 1 && Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_INTERRUPTED));
  assert(g_events[0].host_time_ns > 0);
  assert(strlen(g_events[0].device_name) == 255);
  assert(strlen(g_events[0].message) == 511);
  mrec_track_resumed(g_session, MREC_TRACK_MICROPHONE, 0, long_name);
  mrec_track_resumed(g_session, MREC_TRACK_MICROPHONE, 0, long_name);
  assert(g_count == 2 && Is(1, MREC_TRACK_MICROPHONE, MREC_TRACK_RESUMED));
  char name[2] = {'x', 'x'};
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_OK);
  assert(name[0] == 'a' && name[1] == '\0');
  assert(mrec_current_microphone_name(NULL, 2) == MREC_ERR_INTERNAL);
  assert(mrec_current_microphone_name(name, 0) == MREC_ERR_INTERNAL);
  assert(mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0,
                            MREC_TRACK_REASON_DEVICE_CHANGED, NULL,
                            UnterminatedName, NULL));
  assert(g_count == 3 && Is(2, MREC_TRACK_MICROPHONE, MREC_TRACK_DEVICE_CHANGED));
  assert(strlen(g_events[2].device_name) == 255);
  mrec_tracks_close();
}

static void ReplacementSessionCancelsOldRecovery(void) {
  Reset();
  for (int result = 0; result <= 1; result++) {
    assert(!mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0,
                               MREC_TRACK_REASON_UNKNOWN, NULL, ReplaceSession, &result));
    assert(g_count == 0);
  }
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
                         MREC_TRACK_REASON_APP_EXITED, "exited");
  mrec_track_resumed(g_session, MREC_TRACK_SYSTEM_AUDIO, 0, "Late source");
  Source source = {0, 2, 0, 0, "Old device"};
  assert(!mrec_track_recover(g_session, MREC_TRACK_SYSTEM_AUDIO, 0,
                             MREC_TRACK_REASON_DEVICE_REMOVED, NULL, Attempt, &source));
  assert(!mrec_track_add_source(g_session, MREC_TRACK_SYSTEM_AUDIO));
  assert(source.attempts == 0 && g_count == 1);
  mrec_tracks_close();
}

/* A process started later is a new live source; a lost track becomes degraded, not live. */
static void AddedSourcesJoinTheTrack(void) {
  Reset();
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 1,
                         MREC_TRACK_REASON_SERVICE_RESET, "lost one");
  assert(mrec_track_add_source(g_session, MREC_TRACK_SYSTEM_AUDIO));
  assert(g_count == 1 && Is(0, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_DEGRADED));
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 0,
                         MREC_TRACK_REASON_SERVICE_RESET, "lost two");
  assert(g_count == 1);
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 2,
                         MREC_TRACK_REASON_SERVICE_RESET, "lost all");
  assert(g_count == 2 && Is(1, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_INTERRUPTED));
  assert(mrec_track_add_source(g_session, MREC_TRACK_SYSTEM_AUDIO));
  assert(g_count == 3 && Is(2, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_DEGRADED));
  for (uint32_t source = 0; source < 3; source++) {
    mrec_track_resumed(g_session, MREC_TRACK_SYSTEM_AUDIO, source, NULL);
  }
  assert(g_count == 4 && Is(3, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_RESUMED));
  mrec_tracks_close();
  assert(!mrec_track_add_source(g_session, MREC_TRACK_SYSTEM_AUDIO));
}

/* Retries run on a fixed interval, and at once after a device notification. */
static void RetriesRunOnIntervalOrWake(void) {
  const double interval = MREC_TRACK_RETRY_INTERVAL_MS / 1000.0;
  Reset();
  mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, "Headset");
  Source timed = {0, 3, 0, 0, "Headset"};
  double started = Seconds();
  assert(mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_REMOVED,
                            "unplugged", Attempt, &timed));
  double elapsed = Seconds() - started;
  assert(timed.attempts == 3);
  assert(elapsed >= 2 * interval * 0.9 && elapsed < 2 * interval + 0.5);
  assert(g_count == 2);
  assert(Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_INTERRUPTED));
  assert(g_events[0].reason == MREC_TRACK_REASON_DEVICE_REMOVED);
  assert(strcmp(g_events[0].message, "unplugged") == 0);
  assert(Is(1, MREC_TRACK_MICROPHONE, MREC_TRACK_RESUMED));

  /* A notification during an attempt still wakes the wait that follows it. */
  Reset();
  Source woken = {0, 4, 1, 0, NULL};
  started = Seconds();
  assert(mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_REMOVED,
                            "unplugged", Attempt, &woken));
  elapsed = Seconds() - started;
  assert(woken.attempts == 4 && elapsed < interval);
  assert(g_count == 2 && Is(1, MREC_TRACK_MICROPHONE, MREC_TRACK_RESUMED));

  /* Retries continue until the session stops, and stopping ends them. */
  Reset();
  Source gone = {0, 1000, 1, 8, NULL};
  assert(!mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_REMOVED,
                             "none", Attempt, &gone));
  assert(gone.attempts == 8);
  assert(g_count == 1 && Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_INTERRUPTED));
  mrec_tracks_wake(g_session);
}

static void AppExitSupersedesDeviceInterruption(void) {
  Reset();
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 0,
                         MREC_TRACK_REASON_SERVICE_RESET, "lost first source");
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 1,
                         MREC_TRACK_REASON_SERVICE_RESET, "lost last source");
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
                         MREC_TRACK_REASON_APP_EXITED, "exited");
  assert(g_count == 3 && Is(2, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_INTERRUPTED));
  assert(g_events[2].reason == MREC_TRACK_REASON_APP_EXITED);
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
                         MREC_TRACK_REASON_APP_EXITED, "duplicate exit");
  mrec_track_resumed(g_session, MREC_TRACK_SYSTEM_AUDIO, 0, "Late device");
  assert(g_count == 3);
  mrec_tracks_close();
}

int main(void) {
  InvalidSourcesDoNotReopen();
  DuplicateTransitionsAndNamesAreQuiet();
  ReplacementSessionCancelsOldRecovery();
  AppExitSupersedesDeviceInterruption();
  AddedSourcesJoinTheTrack();
  RetriesRunOnIntervalOrWake();
  /* The Rust binding hardcodes this layout. */
  assert(sizeof(mrec_track_event) == 792);
  assert(offsetof(mrec_track_event, host_time_ns) == 16);
  assert(offsetof(mrec_track_event, device_name) == 24);
  assert(offsetof(mrec_track_event, message) == 280);

  /* A microphone switch that reopens at once is a device change, not an interruption. */
  Reset();
  mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, "Laptop");
  Source quick = {0, 1, 0, 0, "Headset"};
  assert(mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_CHANGED,
                            "changed", Attempt, &quick));
  assert(g_count == 1 && Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_DEVICE_CHANGED));
  assert(strcmp(g_events[0].device_name, "Headset") == 0);
  char name[64];
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_OK);
  assert(strcmp(name, "Headset") == 0);

  /* System audio resumes only once every stopped source is back. */
  Reset();
  mrec_track_started(g_session, MREC_TRACK_SYSTEM_AUDIO, 2, NULL);
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 0, MREC_TRACK_REASON_SERVICE_RESET, NULL);
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, 1, MREC_TRACK_REASON_SERVICE_RESET, NULL);
  mrec_track_resumed(g_session, MREC_TRACK_SYSTEM_AUDIO, 0, NULL);
  assert(g_count == 3);
  assert(Is(0, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_DEGRADED));
  assert(Is(1, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_INTERRUPTED));
  assert(Is(2, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_DEGRADED));
  mrec_track_resumed(g_session, MREC_TRACK_SYSTEM_AUDIO, 1, NULL);
  assert(g_count == 4 && Is(3, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_RESUMED));
  assert(g_events[3].device_name[0] == '\0');

  /* Exited apps interrupt the whole track, whatever its sources do. */
  mrec_track_interrupted(g_session, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
                         MREC_TRACK_REASON_APP_EXITED, "exited");
  mrec_track_resumed(g_session, MREC_TRACK_SYSTEM_AUDIO, 0, NULL);
  assert(g_count == 5 && Is(4, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_INTERRUPTED));
  assert(g_events[4].reason == MREC_TRACK_REASON_APP_EXITED);

  /* Tracks are independent, and nothing is published after close. */
  mrec_track_interrupted(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_UNKNOWN, NULL);
  assert(g_count == 6 && Is(5, MREC_TRACK_MICROPHONE, MREC_TRACK_INTERRUPTED));
  mrec_tracks_close();
  mrec_track_resumed(g_session, MREC_TRACK_MICROPHONE, 0, "Other");
  assert(g_count == 6);
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_ERR_NOT_RUNNING);

  /* Without a callback, state is still kept. */
  g_session = mrec_tracks_open(NULL, NULL);
  mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, "Built-in");
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_OK);
  assert(strcmp(name, "Built-in") == 0);
  mrec_tracks_close();

  /* A retry from a stopped session must not publish into its replacement. */
  const uint64_t previous_session = g_session;
  Reset();
  mrec_track_interrupted(previous_session, MREC_TRACK_MICROPHONE, 0,
                         MREC_TRACK_REASON_DEVICE_REMOVED, "stale interruption");
  mrec_track_resumed(previous_session, MREC_TRACK_MICROPHONE, 0, "Stale device");
  assert(g_count == 0);
  Source stale = {0, 1, 0, 0, "Old microphone"};
  mrec_track_recover(previous_session, MREC_TRACK_MICROPHONE, 0,
                     MREC_TRACK_REASON_UNKNOWN, "stale", Attempt, &stale);
  assert(g_count == 0 && stale.attempts == 0);
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_ERR_NOT_RUNNING);
  for (int result = 0; result <= 1; result++) {
    mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_UNKNOWN,
                       "old attempt", ReplaceSession, &result);
    assert(g_count == 0);
    assert(mrec_current_microphone_name(name, sizeof name) == MREC_ERR_NOT_RUNNING);
  }
  mrec_tracks_close();

  printf("track tests passed\n");
  return 0;
}
