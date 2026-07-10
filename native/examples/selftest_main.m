/*
 * GUI host for the self-test.
 *
 * The audio-capture TCC prompt can only be drawn by a process with a running
 * NSApplication, and the CoreAudio calls that trigger it must not run on the
 * thread that has to draw it. So: boot an accessory (no dock icon) NSApplication
 * on the main thread, and run the actual test on a background thread.
 *
 * Real hosts — Electron, a SwiftUI app — already satisfy this and need none of
 * this scaffolding.
 */
#import <AppKit/AppKit.h>
#include <pthread.h>

#include "meeting-record.h"

/* Defined in selftest.c */
int mrec_selftest_run(void);

static void *run_test(void *unused) {
  (void)unused;

  /*
   * Ask for the grant if we do not already have it, then wait for the user. The
   * prompt is modal to them, not to us, so poll rather than block.
   */
  if (mrec_audio_permission_status() != MREC_PERM_GRANTED) {
    mrec_request_audio_permission();
    for (int i = 0; i < 60; i++) {
      if (mrec_audio_permission_status() == MREC_PERM_GRANTED) break;
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

    /* Safety net: never leave a wedged process holding a tap. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(180 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ exit(3); });

    [NSApp run];
  }
  return 0;
}
