#pragma once

#include <cstdint>
#include <limits>

namespace meeting_record {

constexpr int32_t kCaptureEmpty = 1;
/* HRESULT_FROM_WIN32(ERROR_INVALID_DATA). */
constexpr int32_t kCaptureInvalidPacket = -2147024883;

struct CapturePacket {
  const float *samples = nullptr;
  uint32_t frames = 0;
  uint64_t time_100ns = 0;
  bool silent = false;
  bool discontinuity = false;
  bool timestamp_error = false;
};

struct CaptureReadResult {
  int32_t status = 0;
  bool empty = false;
};

/* Consume WASAPI-owned samples before releasing the packet, including on exceptions. */
template <typename Source, typename Emit, typename Clock>
CaptureReadResult ReadCapturePacket(Source &source, Emit &&emit, Clock &&now) {
  uint32_t available = 0;
  int32_t status = source.NextPacketSize(available);
  if (status < 0) return {status, false};
  if (available == 0) return {0, true};

  CapturePacket packet;
  status = source.Acquire(packet);
  if (status < 0) return {status, false};
  if (status == kCaptureEmpty) return {0, true};

  struct Release {
    Source &source;
    uint32_t frames;
    bool pending = true;

    int32_t Finish() {
      pending = false;
      return source.Release(frames);
    }
    ~Release() {
      if (pending) source.Release(frames);
    }
  } release{source, packet.frames};

  if (packet.frames > available ||
      (packet.frames != 0 && packet.samples == nullptr && !packet.silent)) {
    release.Finish();
    return {kCaptureInvalidPacket, false};
  }
  if (packet.frames != 0) {
    const bool valid_time = !packet.timestamp_error && packet.time_100ns != 0 &&
                           packet.time_100ns <= (std::numeric_limits<uint64_t>::max)() / 100;
    const uint64_t timestamp = valid_time ? packet.time_100ns * 100 : now();
    emit(packet.samples, packet.frames, packet.silent, packet.discontinuity, timestamp);
  }
  return {release.Finish(), false};
}

template <typename Source, typename Emit, typename Clock, typename Recover, typename Wait>
void PumpCapture(Source &source, Emit &&emit, Clock &&now, Recover &&recover, Wait &&wait) {
  const CaptureReadResult result = ReadCapturePacket(source, emit, now);
  if (result.status < 0) {
    recover(result.status);
  } else if (result.empty) {
    wait();
  }
}

} // namespace meeting_record
