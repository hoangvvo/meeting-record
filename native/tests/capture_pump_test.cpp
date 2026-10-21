#include "../windows/CapturePump.h"
#include "../windows/TimelineMixer.h"
#include "tracks.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using meeting_record::CapturePacket;
using meeting_record::kFramesPerBlock;
using meeting_record::PumpCapture;
using meeting_record::TimelineMixer;

struct Packets {
  std::vector<float> samples = std::vector<float>(kFramesPerBlock, 0.25f);
  CapturePacket packet{samples.data(), kFramesPerBlock, 10000000, false, false, false};
  uint32_t available = kFramesPerBlock;
  int32_t next_status = 0;
  int32_t acquire_status = 0;
  int32_t release_status = 0;
  int acquisitions = 0;
  int releases = 0;
  uint32_t released_frames = 0;
  bool held = false;

  int32_t NextPacketSize(uint32_t &frames) {
    assert(!held);
    frames = available;
    return next_status;
  }

  int32_t Acquire(CapturePacket &result) {
    acquisitions++;
    if (acquire_status != 0) return acquire_status;
    assert(!held);
    held = true;
    result = packet;
    return 0;
  }

  int32_t Release(uint32_t frames) {
    assert(held);
    held = false;
    releases++;
    released_frames = frames;
    std::fill(samples.begin(), samples.end(), -0.75f);
    return release_status;
  }
};

struct Block {
  std::vector<float> samples;
  uint64_t time;
};

struct Capture {
  TimelineMixer mixer;
  std::vector<Block> blocks;
  std::vector<int32_t> failures;
  int waits = 0;
  int clock_reads = 0;

  Capture() {
    mixer.Reset(1, 1, [](const float *frames, uint32_t count, uint32_t channels,
                          double rate, uint64_t time, void *context) {
      assert(count == kFramesPerBlock && channels == 1 && rate == 48000);
      auto &capture = *static_cast<Capture *>(context);
      capture.blocks.push_back({std::vector<float>(frames, frames + count), time});
    }, this);
  }

  void Poll(Packets &source) {
    PumpCapture(source,
        [this, &source](const float *samples, uint32_t frames, bool silent,
                        bool discontinuity, uint64_t time) {
          assert(source.held);
          mixer.Add(0, samples, frames, silent, discontinuity, time);
        },
        [this] { clock_reads++; return uint64_t{2000000000}; },
        [this, &source](int32_t failure) {
          assert(!source.held);
          failures.push_back(failure);
        },
        [this] { waits++; });
  }
};

bool All(const Block &block, float value) {
  return std::all_of(block.samples.begin(), block.samples.end(),
                     [value](float sample) { return sample == value; });
}

void SamplesAreConsumedBeforeRelease() {
  Packets source;
  Capture capture;
  capture.Poll(source);
  assert(source.releases == 1 && source.released_frames == kFramesPerBlock);
  assert(capture.blocks.size() == 1 && All(capture.blocks[0], 0.25f));
  assert(capture.blocks[0].time == 1000000000);
  assert(source.samples[0] == -0.75f && capture.clock_reads == 0);
  assert(capture.failures.empty() && capture.waits == 0);
}

void SilentPacketsAllowNullBuffers() {
  Packets source;
  source.packet.samples = nullptr;
  source.packet.silent = true;
  Capture capture;
  capture.Poll(source);
  assert(source.releases == 1 && !source.held);
  assert(capture.blocks.size() == 1 && All(capture.blocks[0], 0));
  assert(capture.failures.empty());
}

void MalformedPacketsAreReleasedBeforeRecovery() {
  for (bool null_buffer : {true, false}) {
    Packets source;
    if (null_buffer) source.packet.samples = nullptr;
    else source.packet.frames++;
    Capture capture;
    capture.Poll(source);
    assert(source.releases == 1 && source.released_frames == source.packet.frames);
    assert(capture.blocks.empty() && capture.clock_reads == 0);
    assert(capture.failures == std::vector<int32_t>{meeting_record::kCaptureInvalidPacket});
  }
}

void EmptyPollAndAcquireRaceWaitWithoutRelease() {
  for (bool acquire_race : {false, true}) {
    Packets source;
    if (acquire_race) source.acquire_status = meeting_record::kCaptureEmpty;
    else source.available = 0;
    Capture capture;
    capture.Poll(source);
    assert(source.acquisitions == (acquire_race ? 1 : 0));
    assert(source.releases == 0 && capture.waits == 1);
    assert(capture.blocks.empty() && capture.failures.empty());
  }
}

void PacketFailuresReachRecovery() {
  for (int stage = 0; stage < 3; stage++) {
    Packets source;
    if (stage == 0) source.next_status = -101;
    if (stage == 1) source.acquire_status = -101;
    if (stage == 2) source.release_status = -101;
    Capture capture;
    capture.Poll(source);
    assert(capture.failures == std::vector<int32_t>{-101});
    assert(source.releases == (stage == 2 ? 1 : 0));
    assert(capture.blocks.size() == (stage == 2 ? 1u : 0u));
    assert(capture.waits == 0 && !source.held);
  }
}

void EmptyAcquiredPacketStillReleases() {
  Packets source;
  source.packet.frames = 0;
  source.packet.samples = nullptr;
  Capture capture;
  capture.Poll(source);
  assert(source.releases == 1 && source.released_frames == 0);
  assert(capture.blocks.empty() && capture.clock_reads == 0);
  assert(capture.failures.empty() && capture.waits == 0);
}

void InvalidTimestampsUseTheHostClock() {
  for (int failure = 0; failure < 3; failure++) {
    Packets source;
    if (failure == 0) source.packet.time_100ns = 0;
    if (failure == 1) source.packet.timestamp_error = true;
    if (failure == 2) source.packet.time_100ns = UINT64_MAX;
    Capture capture;
    capture.Poll(source);
    assert(capture.clock_reads == 1 && capture.blocks.size() == 1);
    assert(capture.blocks[0].time == 2000000000);
  }
}

void DiscontinuityPreservesTheReportedGap() {
  Packets source;
  Capture capture;
  capture.Poll(source);
  std::fill(source.samples.begin(), source.samples.end(), 0.5f);
  source.packet.time_100ns += 200000;
  source.packet.discontinuity = true;
  capture.Poll(source);
  assert(capture.blocks.size() == 3);
  assert(All(capture.blocks[0], 0.25f));
  assert(All(capture.blocks[1], 0));
  assert(All(capture.blocks[2], 0.5f));
  assert(capture.blocks[1].time == 1010000000);
  assert(capture.blocks[2].time == 1020000000);
}

void ThrowingConsumerStillReleases() {
  Packets source;
  try {
    PumpCapture(source,
        [](const float *, uint32_t, bool, bool, uint64_t) { throw std::runtime_error("consumer"); },
        [] { return uint64_t{0}; },
        [](int32_t) { assert(false); }, [] { assert(false); });
    assert(false);
  } catch (const std::runtime_error &) {
    assert(source.releases == 1 && !source.held);
  }
}

struct Recovery {
  Packets &source;
  int attempts = 0;
  bool cancel = false;
  std::vector<uint32_t> delays;

  static int32_t Reopen(void *context, char *, size_t) {
    auto &state = *static_cast<Recovery *>(context);
    assert(!state.source.held);
    if (++state.attempts < 3) return 0;
    state.source.next_status = 0;
    return 1;
  }

  static int32_t Wait(void *context, uint32_t delay) {
    auto &state = *static_cast<Recovery *>(context);
    state.delays.push_back(delay);
    return state.cancel ? 0 : 1;
  }
};

void PumpFailuresUseSharedRecoveryAndResumeAudio() {
  for (bool cancel : {false, true}) {
    Packets source;
    source.next_status = -101;
    Capture capture;
    Recovery recovery{source, 0, cancel, {}};
    std::vector<mrec_track_event_type> events;
    const auto session = mrec_tracks_open([](const mrec_track_event *event, void *context) {
      static_cast<std::vector<mrec_track_event_type> *>(context)->push_back(event->type);
    }, &events);
    assert(mrec_track_started(session, MREC_TRACK_SYSTEM_AUDIO, 1, nullptr));
    PumpCapture(source,
        [](const float *, uint32_t, bool, bool, uint64_t) { assert(false); },
        [] { return uint64_t{0}; },
        [&recovery, session](int32_t failure) {
          assert(failure == -101);
          const auto resumed = mrec_track_recover(
              session, MREC_TRACK_SYSTEM_AUDIO, 0, MREC_TRACK_REASON_DEVICE_REMOVED,
              "capture failed", Recovery::Reopen, Recovery::Wait, &recovery);
          assert(resumed == (recovery.cancel ? 0 : 1));
        }, [] { assert(false); });
    assert(events.front() == MREC_TRACK_INTERRUPTED);
    if (cancel) {
      assert(recovery.attempts == 1 && events.size() == 1);
      assert(recovery.delays == std::vector<uint32_t>{100});
    } else {
      assert(recovery.attempts == 3 && events.size() == 2);
      assert(events.back() == MREC_TRACK_RESUMED);
      assert(recovery.delays == (std::vector<uint32_t>{100, 200}));
      capture.Poll(source);
      assert(capture.blocks.size() == 1 && All(capture.blocks[0], 0.25f));
    }
    mrec_tracks_close();
  }
}

} // namespace

int main() {
  SamplesAreConsumedBeforeRelease();
  SilentPacketsAllowNullBuffers();
  MalformedPacketsAreReleasedBeforeRecovery();
  EmptyPollAndAcquireRaceWaitWithoutRelease();
  PacketFailuresReachRecovery();
  EmptyAcquiredPacketStillReleases();
  InvalidTimestampsUseTheHostClock();
  DiscontinuityPreservesTheReportedGap();
  ThrowingConsumerStillReleases();
  PumpFailuresUseSharedRecoveryAndResumeAudio();
  std::cout << "capture pump tests passed\n";
}
