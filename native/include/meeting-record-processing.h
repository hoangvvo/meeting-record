#pragma once
#include <stdint.h>

/* OR with microphone source DEFAULT or AUTO. Effects default off. Echo
 * cancellation references only captured system audio. Input rates: 8–192 kHz;
 * processed microphone output is mono. */
typedef enum {
  MREC_MICROPHONE_ECHO_CANCELLATION = 0x100,
  MREC_MICROPHONE_NOISE_SUPPRESSION = 0x200,
  MREC_MICROPHONE_AUTO_GAIN_CONTROL = 0x400,
} mrec_microphone_processing;
