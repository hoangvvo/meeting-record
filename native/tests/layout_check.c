/*
 * Guards the hand-written struct offsets in native/macos/MeetingExports.swift,
 * which writes fields at literal byte offsets because Swift cannot import the
 * header. A header change fails here at compile time.
 */
#include "meeting-record-detect.h"

#include <stddef.h>
#include <stdio.h>
#define CK(field, expected) \
  _Static_assert(offsetof(mrec_meeting, field) == (expected), \
                 "offset mismatch: " #field)
CK(platform, 0);
CK(pid, 4);
CK(audio_pids, 8);
CK(audio_pid_count, 72);
CK(app_name, 80);
CK(title, 208);
CK(url, 720);
CK(is_using_mic, 1744);
CK(is_playing_audio, 1748);
CK(confidence, 1752);
CK(should_record, 1756);
CK(detected_at_ns, 1760);
_Static_assert(sizeof(mrec_meeting) == 1768, "size mismatch");
int main(void) {
  printf("mrec_meeting layout OK (sizeof=%zu)\n", sizeof(mrec_meeting));
  return 0;
}
