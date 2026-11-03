#pragma once
#include "../include/meeting-record-processing.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MREC_MICROPHONE_PROCESSING_MASK 0x700
static inline int32_t mrec_voice_rate_supported(double rate) { return rate >= 8000 && rate <= 192000; }
static inline int32_t mrec_microphone_source_value(int32_t value) { return value & 0xff; }
static inline int32_t mrec_microphone_processing_value(int32_t value) { return value & MREC_MICROPHONE_PROCESSING_MASK; }
static inline int32_t mrec_microphone_configuration_valid(int32_t value) {
  const int32_t source = mrec_microphone_source_value(value);
  return (value & ~(MREC_MICROPHONE_PROCESSING_MASK | 0xff)) == 0 &&
      source >= 0 && source <= 2 && (source != 0 || value == 0);
}

typedef void (*mrec_voice_callback)(const float *, uint32_t, uint32_t, double, uint64_t, void *);

typedef struct mrec_voice_processor mrec_voice_processor;

/* Route both captured tracks through these callbacks. System PCM is unchanged.
 * Stop both producers before flush/destroy; flush emits any pending mic tail.
 * Mic callbacks are serialized and must not reenter these processing functions. */
mrec_voice_processor *mrec_voice_create(int32_t flags,
    mrec_voice_callback system_callback, void *system_data,
    mrec_voice_callback microphone_callback, void *microphone_data);
/* Set before starting producers. Accounts for backend mixer lookahead. */
void mrec_voice_set_reference_wait(mrec_voice_processor *, uint32_t milliseconds);
void mrec_voice_system(const float *, uint32_t, uint32_t, double, uint64_t, void *);
void mrec_voice_microphone(const float *, uint32_t, uint32_t, double, uint64_t, void *);
void mrec_voice_reset_microphone(mrec_voice_processor *);
void mrec_voice_flush(mrec_voice_processor *);
void mrec_voice_destroy(mrec_voice_processor *);

#ifdef __cplusplus
}
#endif
