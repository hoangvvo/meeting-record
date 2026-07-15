/*
 * End-to-end check: enumerate audio processes, capture the ones playing, report
 * how much non-silent PCM arrived.
 *
 * Requires a signed .app bundle with NSAudioCaptureUsageDescription and a running
 * NSApplication (see selftest_main.m).
 */
#include "meeting-record.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
  unsigned long long frames;
  double peak;
  double sum_squares;
  unsigned long long samples;
  double sample_rate;
  unsigned channels;
} g;

static void on_audio(const float *frames, uint32_t frame_count, uint32_t channels,
                     double sample_rate, uint64_t host_time_ns, void *user_data) {
  (void)host_time_ns;
  (void)user_data;
  g.frames += frame_count;
  g.sample_rate = sample_rate;
  g.channels = channels;
  size_t n = (size_t)frame_count * channels;
  for (size_t i = 0; i < n; i++) {
    double v = fabs((double)frames[i]);
    if (v > g.peak) g.peak = v;
    g.sum_squares += v * v;
    g.samples++;
  }
}

int mrec_selftest_run(void) {
  FILE *log = fopen("/tmp/meeting-record-selftest.log", "w");
  if (!log) return 1;
  setvbuf(log, NULL, _IOLBF, 0);

  fprintf(log, "permission status: %d (1=granted 2=denied 0=unknown)\n",
          mrec_audio_permission_status());

  static mrec_process procs[128];
  size_t count = 0;
  mrec_status st = mrec_list_audio_processes(procs, 128, &count);
  fprintf(log, "list_audio_processes -> %d, count=%zu\n", st, count);

  uint32_t playing[128];
  size_t playing_count = 0;
  for (size_t i = 0; i < count; i++) {
    if (procs[i].is_running_output) {
      fprintf(log, "  PLAYING pid=%u %s (%s)\n", procs[i].pid, procs[i].name,
              procs[i].bundle_id);
      playing[playing_count++] = procs[i].pid;
    }
  }
  fprintf(log, "processes currently playing: %zu\n", playing_count);

  mrec_config cfg;
  mrec_config_defaults(&cfg);
  cfg.mono = 0;
  /* MREC_PIDS="123,456" pins capture to specific processes. */
  static uint32_t explicit_pids[32];
  size_t explicit_count = 0;
  char pid_buf[256] = {0};
  const char *pid_env = getenv("MREC_PIDS");
  if (!pid_env) {
    FILE *opts = fopen("/tmp/meeting-record-test-pids", "r");
    if (opts) {
      if (fgets(pid_buf, sizeof pid_buf, opts)) pid_env = pid_buf;
      fclose(opts);
    }
  }
  if (pid_env) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", pid_env);
    for (char *tok = strtok(buf, ","); tok && explicit_count < 32;
         tok = strtok(NULL, ",")) {
      explicit_pids[explicit_count++] = (uint32_t)strtoul(tok, NULL, 10);
    }
    cfg.pids = explicit_pids;
    cfg.pid_count = explicit_count;
    fprintf(log, "mode: explicit pids (%zu)\n", explicit_count);
  }

  /* MREC_GLOBAL=1 captures the whole-system mix instead. */
  if (getenv("MREC_GLOBAL") || access("/tmp/meeting-record-test-global", F_OK) == 0) {
    cfg.global_mixdown = 1;
    fprintf(log, "mode: GLOBAL mixdown\n");
  } else {
    fprintf(log, "mode: per-process\n");
  }
  /* NULL pids captures every actively-playing process. */

  st = mrec_start(&cfg, on_audio, NULL);
  fprintf(log, "mrec_start -> %d (%s)\n", st, mrec_last_error());
  if (st != MREC_OK) {
    fclose(log);
    return 2;
  }

  double rate = 0;
  uint32_t ch = 0;
  mrec_current_format(&rate, &ch);
  fprintf(log, "negotiated format: %.0f Hz, %u ch, running=%d\n", rate, ch,
          mrec_is_running());

  for (int s = 1; s <= 6; s++) {
    sleep(1);
    fprintf(log, "  t=%ds frames=%llu peak=%.6f\n", s, g.frames, g.peak);
  }

  fprintf(log, "mrec_stop -> %d\n", mrec_stop());

  double rms = g.samples ? sqrt(g.sum_squares / (double)g.samples) : 0.0;
  double seconds = g.sample_rate > 0 ? (double)g.frames / g.sample_rate : 0.0;
  fprintf(log, "RESULT frames=%llu seconds=%.2f channels=%u peak=%.6f rms=%.6f\n",
          g.frames, seconds, g.channels, g.peak, rms);
  fprintf(log, "VERDICT %s\n",
          (g.frames > 0 && g.peak > 0.0) ? "PASS (real audio captured)"
                                         : (g.frames > 0 ? "FRAMES BUT SILENT"
                                                         : "NO FRAMES"));
  fclose(log);
  return 0;
}
