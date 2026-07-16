/*
 * GUI host for the self-test.
 *
 * The audio-capture prompt requires a running NSApplication, and the CoreAudio
 * calls that trigger it must not run on the thread that draws it. So the app runs
 * on the main thread and the test on a background thread.
 *
 * Electron and native apps already satisfy this.
 */
#import <AppKit/AppKit.h>
#include <pthread.h>

#include "meeting-record.h"

/* Defined in selftest.c */
int mrec_selftest_run(void);

static void *run_test(void *unused) {
  (void)unused;

  /* The prompt is modal to the user, not to this process, so poll. */
  if (mrec_audio_permission_status() != MREC_PERM_GRANTED) {
    mrec_request_audio_permission();
    for (int i = 0; i < 60; i++) {
      if (mrec_audio_permission_status() == MREC_PERM_GRANTED) break;
      [NSThread sleepForTimeInterval:1.0];
    }
  }

  if (mrec_microphone_permission_status() != MREC_PERM_GRANTED) {
    mrec_request_microphone_permission();
    for (int i = 0; i < 60; i++) {
      if (mrec_microphone_permission_status() == MREC_PERM_GRANTED) break;
      [NSThread sleepForTimeInterval:1.0];
    }
  }

  int rc = mrec_selftest_run();
  exit(rc);
}

int main(void) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    pthread_t thread;
    pthread_create(&thread, NULL, run_test, NULL);
    pthread_detach(thread);

    /* Never leave a wedged process holding a tap. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(180 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ exit(3); });

    [NSApp run];
  }
  return 0;
}
