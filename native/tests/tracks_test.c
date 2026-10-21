#include "tracks.h"
#include "meeting-record.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

/* A source that reopens on attempt `succeed_on`, and a wait that stops after `waits`. */
typedef struct {
  int attempts;
  int succeed_on;
  int waits;
  int max_waits;
  uint32_t delays[16];
  const char *device_name;
} Source;

static int32_t Attempt(void *context, char *device_name, size_t capacity) {
  Source *source = (Source *)context;
  if (++source->attempts < source->succeed_on) return 0;
  if (source->device_name) snprintf(device_name, capacity, "%s", source->device_name);
  return 1;
}

static int32_t Wait(void *context, uint32_t milliseconds) {
  Source *source = (Source *)context;
  if (source->waits < 16) source->delays[source->waits] = milliseconds;
  return ++source->waits <= source->max_waits;
}

static int32_t ReplaceSession(void *context, char *device_name, size_t capacity) {
  Reset();
  snprintf(device_name, capacity, "%s", "Old microphone");
  return *(int *)context;
}

static int32_t StopWaiting(void *context, uint32_t milliseconds) {
  (void)context;
  (void)milliseconds;
  return 0;
}

int main(void) {
  /* The Rust binding hardcodes this layout. */
  assert(sizeof(mrec_track_event) == 792);
  assert(offsetof(mrec_track_event, host_time_ns) == 16);
  assert(offsetof(mrec_track_event, device_name) == 24);
  assert(offsetof(mrec_track_event, message) == 280);

  /* A microphone switch that reopens at once is a device change, not an interruption. */
  Reset();
  mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, "Laptop");
  Source quick = {0, 1, 0, 10, {0}, "Headset"};
  assert(mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_CHANGED,
                            "changed", Attempt, Wait, &quick));
  assert(g_count == 1 && Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_DEVICE_CHANGED));
  assert(strcmp(g_events[0].device_name, "Headset") == 0);
  char name[64];
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_OK);
  assert(strcmp(name, "Headset") == 0);

  /* A lost source interrupts once, backs off, and resumes; the device is unchanged. */
  Reset();
  mrec_track_started(g_session, MREC_TRACK_MICROPHONE, 1, "Headset");
  Source slow = {0, 4, 0, 10, {0}, "Headset"};
  assert(mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_REMOVED,
                            "unplugged", Attempt, Wait, &slow));
  assert(slow.attempts == 4 && slow.waits == 3);
  assert(slow.delays[0] == 100 && slow.delays[1] == 200 && slow.delays[2] == 400);
  assert(g_count == 2);
  assert(Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_INTERRUPTED));
  assert(g_events[0].reason == MREC_TRACK_REASON_DEVICE_REMOVED);
  assert(strcmp(g_events[0].message, "unplugged") == 0);
  assert(Is(1, MREC_TRACK_MICROPHONE, MREC_TRACK_RESUMED));

  /* Retries continue until the session stops. */
  Reset();
  Source gone = {0, 1000, 0, 7, {0}, NULL};
  assert(!mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_DEVICE_REMOVED,
                             "none", Attempt, Wait, &gone));
  assert(gone.waits == 8 && gone.attempts == 8);
  assert(gone.delays[4] == 1600 && gone.delays[5] == 2000 && gone.delays[7] == 2000);
  assert(g_count == 1 && Is(0, MREC_TRACK_MICROPHONE, MREC_TRACK_INTERRUPTED));

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
  Source stale = {0, 1, 0, 0, {0}, "Old microphone"};
  mrec_track_recover(previous_session, MREC_TRACK_MICROPHONE, 0,
                     MREC_TRACK_REASON_UNKNOWN, "stale", Attempt, Wait, &stale);
  assert(g_count == 0 && stale.attempts == 0);
  assert(mrec_current_microphone_name(name, sizeof name) == MREC_ERR_NOT_RUNNING);
  for (int result = 0; result <= 1; result++) {
    mrec_track_recover(g_session, MREC_TRACK_MICROPHONE, 0, MREC_TRACK_REASON_UNKNOWN,
                       "old attempt", ReplaceSession, StopWaiting, &result);
    assert(g_count == 0);
    assert(mrec_current_microphone_name(name, sizeof name) == MREC_ERR_NOT_RUNNING);
  }
  mrec_tracks_close();

  printf("track tests passed\n");
  return 0;
}
