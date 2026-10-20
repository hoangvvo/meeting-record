#pragma once

#include "meeting-record.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace meeting_record {

/* Every WASAPI client, and the mixed output, run at this format. */
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFramesPerBlock = 480; /* 10 ms at 48 kHz. */
constexpr uint64_t kBlockDurationNs = 10000000;
constexpr uint64_t kMixerWaitBlocks = 3; /* Allow 30 ms for sibling streams. */
/* Timestamp movement still treated as jitter: 20 ms, as in the bindings' mixers. */
constexpr uint64_t kMaxJitterFrames = kSampleRate / 50;

/*
 * WASAPI exposes one process-loopback client per target pid. Their callbacks are
 * independent even though they share a QPC clock, so forwarding them directly
 * would make the public "system" track contain overlapping, out-of-order chunks.
 *
 * Accumulate every source on that shared timeline and emit one ordered 10 ms
 * stream. A short watermark allows sibling process clients to arrive before a
 * block is committed; missing sources are silence. This mirrors the framing and
 * alignment used by production transcription pipelines.
 */
class TimelineMixer {
public:
  void Reset(size_t source_count, uint32_t channels, mrec_audio_callback callback,
             void *user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    blocks_.clear();
    progress_.assign(source_count, 0);
    source_count_ = source_count;
    channels_ = channels;
    callback_ = callback;
    user_data_ = user_data;
    have_timeline_ = false;
    next_output_block_ = 0;
    max_observed_frame_ = 0;
  }

  /* `discontinuity` is WASAPI's report that samples were lost before this packet. */
  void Add(size_t source, const float *samples, uint32_t frame_count, bool silent,
           bool discontinuity, uint64_t host_time_ns) {
    if (frame_count == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (source >= source_count_ || channels_ == 0 || callback_ == nullptr) return;

    uint64_t block_index = host_time_ns / kBlockDurationNs;
    const uint64_t within_block_ns = host_time_ns % kBlockDurationNs;
    uint64_t frame = block_index * kFramesPerBlock +
                     (within_block_ns * kSampleRate + 500000000ull) / 1000000000ull;
    if (frame >= (block_index + 1) * kFramesPerBlock) {
      block_index++;
      frame = block_index * kFramesPerBlock;
    }

    /* Device timestamps jitter by milliseconds while samples stay contiguous. */
    const uint64_t expected = progress_[source];
    if (expected != 0 && !discontinuity) {
      const uint64_t distance = frame > expected ? frame - expected : expected - frame;
      if (distance <= kMaxJitterFrames) frame = expected;
    }

    /* Drop what a backward jump would write over this source's own audio. */
    uint32_t consumed =
        expected > frame
            ? static_cast<uint32_t>((std::min)(expected - frame, uint64_t{frame_count}))
            : 0;
    while (consumed < frame_count) {
      const uint64_t current_frame = frame + consumed;
      const uint64_t current_block = current_frame / kFramesPerBlock;
      const uint32_t offset = static_cast<uint32_t>(current_frame % kFramesPerBlock);
      const uint32_t count =
          (std::min)(frame_count - consumed, kFramesPerBlock - offset);

      if (!have_timeline_) {
        have_timeline_ = true;
        next_output_block_ = current_block;
      } else if (current_block < next_output_block_) {
        /* This packet arrived after its timeline block was already committed. */
        consumed += count;
        continue;
      }

      if (!silent && samples != nullptr) {
        auto [it, inserted] = blocks_.try_emplace(current_block);
        if (inserted) {
          it->second.assign(static_cast<size_t>(kFramesPerBlock) * channels_, 0.0f);
        }
        auto &mixed = it->second;
        for (uint32_t local = 0; local < count; local++) {
          const size_t destination = static_cast<size_t>(offset + local) * channels_;
          const size_t input = static_cast<size_t>(consumed + local) * channels_;
          for (uint32_t channel = 0; channel < channels_; channel++) {
            mixed[destination + channel] += samples[input + channel];
          }
        }
      }
      consumed += count;
    }

    const uint64_t end_frame = frame + frame_count;
    progress_[source] = (std::max)(progress_[source], end_frame);
    max_observed_frame_ = (std::max)(max_observed_frame_, end_frame);
    EmitReady(false);
  }

  void Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    EmitReady(true);
    blocks_.clear();
    progress_.clear();
    source_count_ = 0;
    callback_ = nullptr;
    user_data_ = nullptr;
    have_timeline_ = false;
  }

private:
  bool EverySourceReached(uint64_t frame) const {
    if (progress_.empty()) return false;
    return std::all_of(progress_.begin(), progress_.end(),
                       [frame](uint64_t value) { return value >= frame; });
  }

  void EmitReady(bool flush_all) {
    if (!have_timeline_ || callback_ == nullptr) return;
    const uint64_t last_observed_block =
        max_observed_frame_ == 0 ? 0 : (max_observed_frame_ - 1) / kFramesPerBlock;

    while (next_output_block_ <= last_observed_block) {
      const uint64_t block_end = (next_output_block_ + 1) * kFramesPerBlock;
      const bool beyond_watermark =
          last_observed_block >= next_output_block_ + kMixerWaitBlocks;
      if (!flush_all && !EverySourceReached(block_end) && !beyond_watermark) break;

      auto it = blocks_.find(next_output_block_);
      if (it == blocks_.end()) {
        scratch_.assign(static_cast<size_t>(kFramesPerBlock) * channels_, 0.0f);
      } else {
        scratch_ = std::move(it->second);
        blocks_.erase(it);
        for (float &sample : scratch_) sample = (std::clamp)(sample, -1.0f, 1.0f);
      }
      callback_(scratch_.data(), kFramesPerBlock, channels_, kSampleRate,
                next_output_block_ * kBlockDurationNs, user_data_);
      next_output_block_++;
    }
  }

  std::mutex mutex_;
  std::map<uint64_t, std::vector<float>> blocks_;
  std::vector<uint64_t> progress_;
  std::vector<float> scratch_;
  size_t source_count_ = 0;
  uint32_t channels_ = 0;
  mrec_audio_callback callback_ = nullptr;
  void *user_data_ = nullptr;
  bool have_timeline_ = false;
  uint64_t next_output_block_ = 0;
  uint64_t max_observed_frame_ = 0;
};

} // namespace meeting_record
