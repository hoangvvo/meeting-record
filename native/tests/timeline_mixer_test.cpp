#include "../windows/TimelineMixer.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using meeting_record::kBlockDurationNs;
using meeting_record::kFramesPerBlock;
using meeting_record::kMixerWaitBlocks;
using meeting_record::TimelineMixer;

struct Block {
  std::vector<float> samples;
  uint32_t channels;
  uint64_t host_time_ns;
};

void Collect(const float *frames, uint32_t frame_count, uint32_t channels,
             double sample_rate, uint64_t host_time_ns, void *user_data) {
  assert(frame_count == kFramesPerBlock);
  assert(sample_rate == meeting_record::kSampleRate);
  auto &blocks = *static_cast<std::vector<Block> *>(user_data);
  blocks.push_back({std::vector<float>(frames, frames + frame_count * channels), channels,
                    host_time_ns});
}

std::vector<float> Constant(uint32_t frames, float value, uint32_t channels = 1) {
  return std::vector<float>(static_cast<size_t>(frames) * channels, value);
}

uint64_t BlockTime(uint64_t block) { return block * kBlockDurationNs; }

bool All(const std::vector<float> &samples, size_t begin, size_t end, float value) {
  for (size_t index = begin; index < end; index++) {
    if (samples[index] != value) return false;
  }
  return true;
}

void SingleSourceEmitsWholeBlocksOnTheTimeline() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  const auto packet = Constant(kFramesPerBlock, 0.25f);
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(100));
  assert(blocks.size() == 1);
  assert(blocks[0].host_time_ns == BlockTime(100));
  assert(All(blocks[0].samples, 0, kFramesPerBlock, 0.25f));
  mixer.Flush();
  assert(blocks.size() == 1);
}

void UnalignedPacketsAreReframedWithoutGaps() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  // 300 frames from 3 ms into block 100, then the next 300 frames.
  const auto packet = Constant(300, 0.5f);
  const uint64_t start = BlockTime(100) + 3000000;
  mixer.Add(0, packet.data(), 300, false, false, start);
  assert(blocks.empty());
  mixer.Add(0, packet.data(), 300, false, false, start + 300ull * 1000000000ull / 48000);
  assert(blocks.size() == 1);
  // The first 3 ms precede the first packet and are silence.
  assert(All(blocks[0].samples, 0, 144, 0.0f));
  assert(All(blocks[0].samples, 144, kFramesPerBlock, 0.5f));
  mixer.Flush();
  assert(blocks.size() == 2);
  assert(blocks[1].host_time_ns == BlockTime(101));
  assert(All(blocks[1].samples, 0, 264, 0.5f));
  assert(All(blocks[1].samples, 264, kFramesPerBlock, 0.0f));
}

void ClockJitterKeepsPacketsContiguous() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  const auto packet = Constant(kFramesPerBlock, 0.5f);
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(10));
  // Two frames late on the QPC clock: still the next block, with no hole.
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(11) + 41000);
  assert(blocks.size() == 2);
  assert(blocks[1].host_time_ns == BlockTime(11));
  assert(All(blocks[1].samples, 0, kFramesPerBlock, 0.5f));
  mixer.Flush();
}

void JitteredTimestampsKeepSamplesContiguous() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  const auto packet = Constant(kFramesPerBlock, 0.5f);
  // A microphone stamps one packet 1.9 ms late, then the next on time again.
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(10));
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(11) + 1900000);
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(12));
  assert(blocks.size() == 3);
  for (const auto &block : blocks) {
    // Neither a hole before the late packet nor an overlap after it.
    assert(All(block.samples, 0, kFramesPerBlock, 0.5f));
  }
  mixer.Flush();
}

void DiscontinuityKeepsTheGap() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  const auto packet = Constant(kFramesPerBlock, 0.5f);
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(10));
  // WASAPI lost 10 ms of samples; the timestamp, not continuity, places the next.
  mixer.Add(0, packet.data(), kFramesPerBlock, false, true, BlockTime(12));
  mixer.Flush();
  assert(blocks.size() == 3);
  assert(All(blocks[1].samples, 0, kFramesPerBlock, 0.0f));
  assert(blocks[2].host_time_ns == BlockTime(12));
  assert(All(blocks[2].samples, 0, kFramesPerBlock, 0.5f));
}

void BackwardJumpNeverDoublesASource() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(2, 1, Collect, &blocks);
  const auto packet = Constant(kFramesPerBlock, 0.25f);
  for (uint64_t block = 10; block < 13; block++) {
    mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(block));
  }
  // Stamped 30 ms early, beyond jitter: what it would rewrite is dropped.
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(10));
  for (uint64_t block = 10; block < 13; block++) {
    mixer.Add(1, packet.data(), kFramesPerBlock, false, false, BlockTime(block));
  }
  assert(blocks.size() == 3);
  for (const auto &block : blocks) assert(All(block.samples, 0, kFramesPerBlock, 0.5f));
  mixer.Flush();
}

void SourcesAreSummedOnTheSharedTimelineAndClamped() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(2, 2, Collect, &blocks);
  const auto first = Constant(kFramesPerBlock, 0.75f, 2);
  const auto second = Constant(kFramesPerBlock, 0.5f, 2);
  mixer.Add(0, first.data(), kFramesPerBlock, false, false, BlockTime(5));
  // A block waits for every source within the watermark.
  assert(blocks.empty());
  mixer.Add(1, second.data(), kFramesPerBlock, false, false, BlockTime(5));
  assert(blocks.size() == 1);
  assert(blocks[0].channels == 2);
  assert(All(blocks[0].samples, 0, kFramesPerBlock * 2, 1.0f));
  mixer.Flush();
}

void StalledSourceIsSilenceAfterTheWatermark() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(2, 1, Collect, &blocks);
  const auto packet = Constant(kFramesPerBlock, 0.25f);
  for (uint64_t block = 0; block < kMixerWaitBlocks; block++) {
    mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(20 + block));
  }
  assert(blocks.empty());
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(20 + kMixerWaitBlocks));
  assert(blocks.size() == 1);
  assert(blocks[0].host_time_ns == BlockTime(20));
  assert(All(blocks[0].samples, 0, kFramesPerBlock, 0.25f));
  // The stalled source's late audio for a committed block is discarded.
  mixer.Add(1, packet.data(), kFramesPerBlock, false, false, BlockTime(20));
  mixer.Flush();
  assert(blocks.size() == kMixerWaitBlocks + 1);
  for (size_t index = 0; index < blocks.size(); index++) {
    assert(blocks[index].host_time_ns == BlockTime(20 + index));
    assert(All(blocks[index].samples, 0, kFramesPerBlock, 0.25f));
  }
}

void SilentPacketsAdvanceTheTimeline() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  mixer.Add(0, nullptr, kFramesPerBlock, true, false, BlockTime(7));
  assert(blocks.size() == 1);
  assert(All(blocks[0].samples, 0, kFramesPerBlock, 0.0f));
  const auto packet = Constant(kFramesPerBlock, 0.5f);
  mixer.Add(0, packet.data(), kFramesPerBlock, false, false, BlockTime(8));
  assert(blocks.size() == 2);
  assert(All(blocks[1].samples, 0, kFramesPerBlock, 0.5f));
  mixer.Flush();
}

void FlushEndsTheSession() {
  std::vector<Block> blocks;
  TimelineMixer mixer;
  mixer.Reset(1, 1, Collect, &blocks);
  const auto packet = Constant(100, 0.5f);
  mixer.Add(0, packet.data(), 100, false, false, BlockTime(3));
  mixer.Flush();
  assert(blocks.size() == 1);
  // Callbacks racing with stop must not reach the released callback.
  mixer.Add(0, packet.data(), 100, false, false, BlockTime(4));
  mixer.Flush();
  assert(blocks.size() == 1);
}

} // namespace

int main() {
  SingleSourceEmitsWholeBlocksOnTheTimeline();
  UnalignedPacketsAreReframedWithoutGaps();
  ClockJitterKeepsPacketsContiguous();
  JitteredTimestampsKeepSamplesContiguous();
  DiscontinuityKeepsTheGap();
  BackwardJumpNeverDoublesASource();
  SourcesAreSummedOnTheSharedTimelineAndClamped();
  StalledSourceIsSilenceAfterTheWatermark();
  SilentPacketsAdvanceTheTimeline();
  FlushEndsTheSession();
  std::cout << "timeline mixer tests passed\n";
}
