/*
 * Exercises the meeting-detection module: one scan, then live watching.
 *
 * Detection needs no permission for its decisive signals, so unlike the audio
 * self-test this runs fine as a plain binary. Titles and URLs will be empty
 * without Accessibility, which is the point being demonstrated.
 */
#include "meeting-record.h"
#include "meeting-record-detect.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static FILE *g_log;

static const char *EventName(mrec_event event) {
  switch (event) {
    case MREC_MEETING_STARTED: return "STARTED";
    case MREC_MEETING_UPDATED: return "UPDATED";
    case MREC_MEETING_ENDED: return "ENDED";
  }
  return "?";
}

static void Describe(const mrec_meeting *m, const char *prefix) {
  fprintf(g_log, "%s %s pid=%u confidence=%d mic=%d out=%d\n", prefix,
          mrec_platform_name(m->platform), m->pid, m->confidence,
          m->is_using_mic, m->is_playing_audio);
  fprintf(g_log, "    app=\"%s\"\n", m->app_name);
  if (m->title[0]) fprintf(g_log, "    title=\"%s\"\n", m->title);
  if (m->url[0]) fprintf(g_log, "    url=\"%s\"\n", m->url);
  fprintf(g_log, "    audio pids (feed these to mrec_start):");
  for (size_t i = 0; i < m->audio_pid_count; i++) {
    fprintf(g_log, " %u", m->audio_pids[i]);
  }
  fprintf(g_log, "\n");
  fprintf(g_log, "    -> would record: %s\n",
          m->should_record ? "YES" : "no (below threshold)");
}

static void OnMeeting(const mrec_meeting *m, mrec_event event,
                      void *user_data) {
  (void)user_data;
  char prefix[32];
  snprintf(prefix, sizeof prefix, "[%s]", EventName(event));
  Describe(m, prefix);
  fflush(g_log);
}

int main(void) {
  g_log = fopen("/tmp/meeting-record-detecttest.log", "w");
  if (!g_log) return 1;
  setvbuf(g_log, NULL, _IOLBF, 0);

  mrec_permission ax = mrec_accessibility_permission_status();
  fprintf(g_log, "accessibility: %d (1=granted 2=denied) -- titles/URLs %s\n\n",
          ax, ax == MREC_PERM_GRANTED ? "available" : "will be empty");

  /*
   * Dump the raw input the detector classifies. When a user reports "it missed my
   * call", this is the first thing to look at: the bundle ids are what the
   * catalog prefix-matches against, and the output flags are the liveness signal.
   */
  static mrec_process procs[192];
  size_t proc_count = 0;
  if (mrec_list_audio_processes(procs, 192, &proc_count) == MREC_OK) {
    fprintf(g_log, "audio processes the detector sees (%zu):\n", proc_count);
    for (size_t i = 0; i < proc_count; i++) {
      if (!procs[i].is_running_output && !procs[i].is_running_input) continue;
      fprintf(g_log, "  pid=%-7u out=%d in=%d  %-28s %s\n", procs[i].pid,
              procs[i].is_running_output, procs[i].is_running_input,
              procs[i].name, procs[i].bundle_id);
    }
    fprintf(g_log, "\n");
  }

  static mrec_meeting meetings[8];
  size_t count = 0;
  mrec_status st = mrec_scan(meetings, 8, &count);
  fprintf(g_log, "scan -> %d, %zu meeting(s)\n", st, count);
  for (size_t i = 0; i < count; i++) Describe(&meetings[i], "  *");

  fprintf(g_log, "\nwatching for 20s (join or leave a call to see events)...\n");
  st = mrec_watch_start(OnMeeting, NULL);
  fprintf(g_log, "watch_start -> %d (watching=%d)\n", st,
          mrec_is_watching());
  sleep(20);
  fprintf(g_log, "watch_stop -> %d\n", mrec_watch_stop());

  fclose(g_log);
  return 0;
}
