#pragma once
#include <algorithm>
#include <cstdint>

namespace meeting_record::linux_backend {
// Pulse latency is an estimate in the device clock domain. Updates can jump
// or be stale while the recorded PCM remains contiguous. Anchor to receipt of
// the queued PCM instead: these timestamps describe acquisition by the client,
// not the hardware's recording/playback time. Keep sample continuity until
// reads actually stall; device/server replacement creates a new clock.
class PacketClock {
public:
  uint64_t Stamp(uint64_t now_ns, uint64_t duration_ns, uint64_t queued_ns) {
    uint64_t time;
    if (next_ns_ && now_ns >= last_read_ns_ &&
        now_ns - last_read_ns_ < 1000000000ull) {
      time = next_ns_;
    } else {
      time = now_ns - std::min(now_ns, queued_ns);
    }
    next_ns_ = time + duration_ns;
    last_read_ns_ = now_ns;
    return time;
  }

private:
  uint64_t next_ns_ = 0, last_read_ns_ = 0;
};
} // namespace meeting_record::linux_backend
