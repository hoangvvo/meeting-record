#import <AVFoundation/AVFoundation.h>
#include "meeting-record.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const double frequencies[] = {997, 440, 880};

typedef struct {
  double correlation[3][2];
  uint64_t frames;
} ExclusionMetrics;

static void on_audio(const float *pcm, uint32_t count, uint32_t channels,
                     double rate, uint64_t host_time, void *context) {
  (void)host_time;
  ExclusionMetrics *metrics = context;
  for (uint32_t i = 0; i < count; i++, metrics->frames++) {
    // Allow new audio processes to appear before measuring exclusions.
    if (metrics->frames < (uint64_t)(rate * 2)) continue;
    double sample = 0;
    for (uint32_t channel = 0; channel < channels; channel++) {
      sample += pcm[i * channels + channel] / channels;
    }
    for (int frequency = 0; frequency < 3; frequency++) {
      double phase = 2 * M_PI * frequencies[frequency] * metrics->frames / rate;
      metrics->correlation[frequency][0] += sample * cos(phase);
      metrics->correlation[frequency][1] += sample * sin(phase);
    }
  }
}

int mrec_exclusion_test_run(void) {
  @autoreleasepool {
    const char *directory = getenv("MREC_EXCLUSION_FIXTURES");
    if (!directory) return 1;
    NSString *fixtures = [NSString stringWithUTF8String:directory];
    ExclusionMetrics metrics = {0};
    mrec_config config;
    mrec_config_defaults(&config);
    config.global_mixdown = 1;
    if (mrec_start(&config, on_audio, &metrics) != MREC_OK) {
      fprintf(stderr, "%s\n", mrec_last_error());
      return 1;
    }

    usleep(400000);
    NSURL *file = [NSURL fileURLWithPath:[fixtures stringByAppendingPathComponent:@"own.wav"]];
    AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithContentsOfURL:file error:NULL];
    if (!player || ![player play]) {
      mrec_stop();
      return 1;
    }
    NSTask *child = [NSTask new];
    child.executableURL = [NSURL fileURLWithPath:@"/usr/bin/afplay"];
    child.arguments = @[[fixtures stringByAppendingPathComponent:@"child.wav"]];
    if (![child launchAndReturnError:NULL]) {
      [player stop];
      mrec_stop();
      return 1;
    }

    sleep(5);
    mrec_status status = mrec_stop();
    [player stop];
    if (child.running) [child terminate];
    [child waitUntilExit];

    double external = hypot(metrics.correlation[0][0], metrics.correlation[0][1]);
    double own = hypot(metrics.correlation[1][0], metrics.correlation[1][1]);
    double descendant = hypot(metrics.correlation[2][0], metrics.correlation[2][1]);
    int passed = status == MREC_OK && external > 10 &&
                 own / external < .01 && descendant / external < .01;
    printf("SYSTEM EXCLUSION %s external=%.2f recorder=%.2f child=%.2f frames=%llu\n",
           passed ? "PASS" : "FAIL", external, own, descendant,
           (unsigned long long)metrics.frames);
    return passed ? 0 : 1;
  }
}
