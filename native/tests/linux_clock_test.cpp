#include "../linux/CaptureClock.h"
#include "TimelineMixer.h"
#include <cassert>
#include <iostream>

int main() {
  constexpr uint64_t now = 10000000000ull;

  using meeting_record::linux_backend::PacketClock;
  constexpr uint64_t packet = 10000000;
  PacketClock batch;
  assert(batch.Stamp(now, packet, 3 * packet) == now - 3 * packet);
  assert(batch.Stamp(now, packet, 2 * packet) == now - 2 * packet);
  assert(batch.Stamp(now, packet, packet) == now - packet);
  // A real read stall can establish a new acquisition time.
  assert(batch.Stamp(now + 2'000'000'000, packet, packet) ==
         now + 2'000'000'000 - packet);
  PacketClock backlog;
  assert(backlog.Stamp(now, packet, packet) == now - packet);
  // A delayed callback with queued audio must retain its original sample time.
  assert(backlog.Stamp(now + 2'000'000'000, packet, 2'000'000'000) == now);
  PacketClock early;
  assert(early.Stamp(5, 10, 20) == 0);

  struct Output {
    uint64_t frames = 0, next = 0;
    unsigned gaps = 0;
  } output;
  meeting_record::TimelineMixer mixer;
  mixer.Reset(
      1, 1,
      [](const float *samples, uint32_t frames, uint32_t channels,
         double sample_rate, uint64_t host_time_ns, void *context) {
        auto &out = *static_cast<Output *>(context);
        assert(samples && channels == 1 && sample_rate == 48000);
        for (uint32_t i = 0; i < frames; ++i)
          assert(samples[i] == 0.25f);
        if (out.next && host_time_ns != out.next)
          ++out.gaps;
        out.frames += frames;
        out.next = host_time_ns + uint64_t(frames) * 1000000000ull / 48000;
      },
      &output);
  float samples[480];
  for (auto &sample : samples)
    sample = 0.25f;
  PacketClock clock;
  for (unsigned i = 0; i < 200; ++i) {
    // Replay 500 ms callback batches without losing, duplicating, or dating
    // queued PCM in the future. Device-latency estimates cannot move this
    // clock.
    const uint64_t arrival = now + (i / 50 + 1) * 50 * packet;
    const uint64_t time = clock.Stamp(arrival, packet, (50 - i % 50) * packet);
    assert(time + packet <= arrival);
    mixer.Add(0, samples, 480, false, false, time);
  }
  mixer.Flush();
  assert(output.frames == 200 * 480);
  assert(output.gaps == 0);
  std::cout << "Linux capture clock tests passed\n";
}
