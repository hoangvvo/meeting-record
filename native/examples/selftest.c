/*
 * End-to-end check: enumerate audio processes, capture the ones playing, report
 * how much non-silent PCM arrived.
 *
 * Requires a signed .app bundle with audio and microphone usage descriptions and
 * a running NSApplication (see selftest_main.m).
 */
#include "meeting-record.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct metrics {
  unsigned long long frames;
  double peak;
  double sum_squares;
  unsigned long long samples;
  double sample_rate;
  unsigned channels;
  uint64_t first_host_time_ns;
  uint64_t last_host_time_ns;
  unsigned timestamp_regressions;
};

static struct metrics g_system;
static struct metrics g_microphone;

static int env_enabled(const char *name) {
  const char *value = getenv(name);
  return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void update_metrics(struct metrics *metrics, const float *frames,
                           uint32_t frame_count, uint32_t channels,
                           double sample_rate, uint64_t host_time_ns) {
  if (metrics->frames == 0) {
    metrics->first_host_time_ns = host_time_ns;
  } else if (host_time_ns < metrics->last_host_time_ns) {
    metrics->timestamp_regressions++;
  }
  metrics->last_host_time_ns = host_time_ns;
  metrics->frames += frame_count;
  metrics->sample_rate = sample_rate;
  metrics->channels = channels;
  size_t n = (size_t)frame_count * channels;
  for (size_t i = 0; i < n; i++) {
    double v = fabs((double)frames[i]);
    if (v > metrics->peak) metrics->peak = v;
    metrics->sum_squares += v * v;
    metrics->samples++;
  }
}

static void on_audio(const float *frames, uint32_t frame_count, uint32_t channels,
                     double sample_rate, uint64_t host_time_ns, void *user_data) {
  (void)user_data;
  update_metrics(&g_system, frames, frame_count, channels, sample_rate,
                 host_time_ns);
}

static void on_microphone(const float *frames, uint32_t frame_count,
                          uint32_t channels, double sample_rate,
                          uint64_t host_time_ns, void *user_data) {
  (void)user_data;
  update_metrics(&g_microphone, frames, frame_count, channels, sample_rate,
                 host_time_ns);
}

int mrec_selftest_run(void) {
  FILE *log = fopen("/tmp/meeting-record-selftest.log", "w");
  if (!log) return 1;
  setvbuf(log, NULL, _IOLBF, 0);

  fprintf(log, "permission status: %d (1=granted 2=denied 0=unknown)\n",
          mrec_audio_permission_status());
  fprintf(log, "microphone permission: %d (1=granted 2=denied 0=unknown)\n",
          mrec_microphone_permission_status());

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

  int test_microphone = env_enabled("MREC_TEST_MICROPHONE");

  st = (mrec_status)mrec_start_tracks_raw(
      cfg.pids, cfg.pid_count, cfg.global_mixdown, cfg.mono,
      cfg.mute_captured_output,
      test_microphone ? MREC_MICROPHONE_DEFAULT : MREC_MICROPHONE_NONE,
      on_audio, NULL, test_microphone ? on_microphone : NULL, NULL);
  fprintf(log, "mrec_start_tracks_raw -> %d (%s)\n", st, mrec_last_error());
  if (st != MREC_OK) {
    fclose(log);
    return 2;
  }

  double rate = 0;
  uint32_t ch = 0;
  mrec_current_format(&rate, &ch);
  fprintf(log, "system format: %.0f Hz, %u ch, running=%d\n", rate, ch,
          mrec_is_running());
  if (test_microphone) {
    mrec_current_microphone_format(&rate, &ch);
    fprintf(log, "microphone format: %.0f Hz, %u ch\n", rate, ch);
  }

  unsigned duration = 6;
  const char *duration_env = getenv("MREC_DURATION_SECONDS");
  if (duration_env) {
    unsigned parsed = (unsigned)strtoul(duration_env, NULL, 10);
    if (parsed >= 1 && parsed <= 120) duration = parsed;
  }
  fprintf(log, "capturing for %u seconds; play audio%s now\n", duration,
          test_microphone ? " and speak" : "");
  sleep(duration);

  mrec_status stop_status = mrec_stop();
  fprintf(log, "mrec_stop -> %d\n", stop_status);

  double system_rms = g_system.samples
                          ? sqrt(g_system.sum_squares / (double)g_system.samples)
                          : 0.0;
  double system_seconds = g_system.sample_rate > 0
                              ? (double)g_system.frames / g_system.sample_rate
                              : 0.0;
  double microphone_rms = g_microphone.samples
                              ? sqrt(g_microphone.sum_squares /
                                     (double)g_microphone.samples)
                              : 0.0;
  double microphone_seconds =
      g_microphone.sample_rate > 0
          ? (double)g_microphone.frames / g_microphone.sample_rate
          : 0.0;
  fprintf(log, "SYSTEM frames=%llu seconds=%.2f channels=%u peak=%.6f rms=%.6f "
               "timestamps=%llu..%llu regressions=%u\n",
          g_system.frames, system_seconds, g_system.channels, g_system.peak,
          system_rms, g_system.first_host_time_ns, g_system.last_host_time_ns,
          g_system.timestamp_regressions);
  if (test_microphone) {
    fprintf(log, "MICROPHONE frames=%llu seconds=%.2f channels=%u peak=%.6f "
                 "rms=%.6f timestamps=%llu..%llu regressions=%u\n",
            g_microphone.frames, microphone_seconds, g_microphone.channels,
            g_microphone.peak, microphone_rms,
            g_microphone.first_host_time_ns, g_microphone.last_host_time_ns,
            g_microphone.timestamp_regressions);
  }

  double minimum_peak = 0.0001;
  const char *minimum_peak_env = getenv("MREC_MIN_PEAK");
  if (minimum_peak_env) minimum_peak = strtod(minimum_peak_env, NULL);
  int system_pass = stop_status == MREC_OK &&
                    system_seconds >= (double)duration * 0.7 &&
                    g_system.peak >= minimum_peak &&
                    g_system.first_host_time_ns != 0 &&
                    g_system.timestamp_regressions == 0;
  int microphone_pass = !test_microphone ||
                        (microphone_seconds >= (double)duration * 0.7 &&
                         g_microphone.peak >= minimum_peak &&
                         g_microphone.first_host_time_ns != 0 &&
                         g_microphone.timestamp_regressions == 0);
  fprintf(log, "SYSTEM VERDICT %s\n", system_pass ? "PASS" : "FAIL");
  if (test_microphone) {
    fprintf(log, "MICROPHONE VERDICT %s\n",
            microphone_pass ? "PASS" : "FAIL");
  }
  fclose(log);
  if (!system_pass) return 4;
  if (!microphone_pass) return 5;
  return 0;
}
