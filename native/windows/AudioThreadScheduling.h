#pragma once

#include <windows.h>
#include <avrt.h>

namespace meeting_record {

// Register only the worker that owns the audio buffers, and release on that thread.
class AudioThreadScheduling {
public:
  AudioThreadScheduling() {
    DWORD task = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (handle_) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
  }
  ~AudioThreadScheduling() {
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
  }
  AudioThreadScheduling(const AudioThreadScheduling &) = delete;
  AudioThreadScheduling &operator=(const AudioThreadScheduling &) = delete;

private:
  HANDLE handle_ = nullptr;
};

} // namespace meeting_record
