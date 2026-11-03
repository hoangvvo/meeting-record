#include "voice-processing.h"
#include "../third_party/speexdsp/config.h"
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {
constexpr uint64_t kReferenceWaitNs = 40000000;
constexpr uint64_t kHistoryNs = 2000000000;
constexpr size_t kReferenceSamples = 384000;
uint64_t Duration(size_t frames, double rate) {
  return static_cast<uint64_t>(std::llround(frames * 1e9 / rate));
}
float Finite(float sample) { return std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0.f; }
int16_t PCM(float sample) { return static_cast<int16_t>(std::lround(Finite(sample) * 32767.f)); }
bool Valid(const float *samples, uint32_t frames, uint32_t channels, double rate) {
  return samples && frames && frames <= 1048576 && channels && channels <= 32 &&
      std::isfinite(rate) && mrec_voice_rate_supported(rate);
}
struct Reference {
  uint64_t time;
  double rate;
  std::vector<float> samples;
  uint64_t End() const { return time + Duration(samples.size(), rate); }
};
struct Frame {
  uint64_t time;
  double rate;
  size_t valid;
  std::vector<int16_t> samples;
};
struct Output {
  uint64_t time;
  double rate;
  std::vector<float> samples;
};
} // namespace

struct mrec_voice_processor {
  int32_t flags;
  mrec_voice_callback system_callback;
  void *system_data;
  mrec_voice_callback microphone_callback;
  void *microphone_data;
  std::mutex mutex;
  std::mutex delivery_mutex;
  std::deque<Reference> reference;
  std::deque<Frame> pending;
  std::deque<Output> output;
  std::vector<int16_t> partial;
  std::optional<Frame> delayed;
  SpeexEchoState *echo = nullptr;
  SpeexPreprocessState *preprocess = nullptr;
  double rate = 0;
  size_t frame_size = 0;
  uint64_t partial_time = 0;
  uint64_t next_mic_time = 0;
  uint64_t mic_origin = 0;
  size_t mic_received = 0;
  bool have_mic_timeline = false;
  uint64_t latest_time = 0;
  uint64_t reference_wait_ns = kReferenceWaitNs;
  size_t reference_samples = 0;

  mrec_voice_processor(int32_t f, mrec_voice_callback system, void *sd,
                       mrec_voice_callback microphone, void *md)
      : flags(f), system_callback(system), system_data(sd),
        microphone_callback(microphone), microphone_data(md) {}
  ~mrec_voice_processor() { DestroyDSP(); }

  void DestroyDSP() {
    if (preprocess) speex_preprocess_state_destroy(preprocess);
    if (echo) speex_echo_state_destroy(echo);
    preprocess = nullptr;
    echo = nullptr;
    rate = 0;
    frame_size = 0;
    next_mic_time = 0;
    have_mic_timeline = false;
    mic_received = 0;
  }

  void Configure(double sample_rate) {
    rate = sample_rate;
    // Power-of-two FFTs also support unusual device rates whose 10 ms frame
    // would otherwise contain a radix unsupported by the bundled FFT.
    frame_size = 1;
    while (frame_size < static_cast<size_t>(std::llround(rate / 100))) frame_size *= 2;
    partial.reserve(frame_size);
    int frequency = static_cast<int>(std::llround(rate));
    if (flags & MREC_MICROPHONE_ECHO_CANCELLATION) {
      echo = speex_echo_state_init(static_cast<int>(frame_size), static_cast<int>(frame_size * 20));
      speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &frequency);
    }
    if (flags & (MREC_MICROPHONE_NOISE_SUPPRESSION | MREC_MICROPHONE_AUTO_GAIN_CONTROL)) {
      preprocess = speex_preprocess_state_init(static_cast<int>(frame_size), frequency);
      int denoise = (flags & MREC_MICROPHONE_NOISE_SUPPRESSION) != 0;
      int agc = (flags & MREC_MICROPHONE_AUTO_GAIN_CONTROL) != 0;
      speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
      speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_AGC, &agc);
      if (echo) speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
    }
  }

  float ReferenceAt(uint64_t time) const {
    const auto after = std::upper_bound(reference.begin(), reference.end(), time,
        [](uint64_t t, const Reference &r) { return t < r.time; });
    if (after == reference.begin()) return 0;
    const auto &chunk = *std::prev(after);
    if (time >= chunk.End()) return 0;
    const double position = (time - chunk.time) * chunk.rate / 1e9;
    const size_t index = static_cast<size_t>(position);
    const float first = chunk.samples[index];
    float second = first;
    if (index + 1 < chunk.samples.size()) second = chunk.samples[index + 1];
    else if (after != reference.end() && after->time <= chunk.End() + 1000000)
      second = after->samples.front();
    return first + (second - first) * static_cast<float>(position - index);
  }

  void QueueOutput(const Frame &metadata, const std::vector<int16_t> &samples) {
    Output result{metadata.time, metadata.rate, std::vector<float>(metadata.valid)};
    for (size_t i = 0; i < metadata.valid; ++i) result.samples[i] = samples[i] / 32768.f;
    output.push_back(std::move(result));
  }

  void Process(Frame frame) {
    std::vector<int16_t> processed(frame_size);
    if (echo) {
      std::vector<int16_t> far_end(frame_size);
      for (size_t i = 0; i < frame_size; ++i)
        far_end[i] = PCM(ReferenceAt(frame.time + Duration(i, frame.rate)));
      speex_echo_cancellation(echo, frame.samples.data(), far_end.data(), processed.data());
    } else processed = frame.samples;
    if (preprocess) {
      speex_preprocess_run(preprocess, processed.data());
      // The overlap-add preprocessor delays one frame. Assign output to that
      // frame's timestamp, dropping startup padding and flushing the final tail.
      if (delayed) QueueOutput(*delayed, processed);
      delayed = Frame{frame.time, frame.rate, frame.valid, {}};
    } else QueueOutput(frame, processed);
  }

  void Drain(bool final) {
    while (!pending.empty()) {
      const auto end = pending.front().time + Duration(frame_size, pending.front().rate);
      bool ready = !(flags & MREC_MICROPHONE_ECHO_CANCELLATION) || final;
      if (!reference.empty() && reference.back().End() >= end) ready = true;
      if (latest_time >= end && latest_time - end >= reference_wait_ns) ready = true;
      if (!ready) break;
      Frame frame = std::move(pending.front());
      pending.pop_front();
      Process(std::move(frame));
    }
  }

  void FlushLocked() {
    if (!partial.empty()) {
      const auto valid = partial.size();
      partial.resize(frame_size, 0);
      pending.push_back(Frame{partial_time, rate, valid, std::move(partial)});
      partial.clear();
    }
    Drain(true);
    if (preprocess && delayed) {
      std::vector<int16_t> zero(frame_size);
      speex_preprocess_run(preprocess, zero.data());
      QueueOutput(*delayed, zero);
      delayed.reset();
    }
  }

  void DeliverLocked() {
    std::unique_lock<std::mutex> lock(mutex);
    while (!output.empty()) {
      Output chunk = std::move(output.front());
      output.pop_front();
      lock.unlock();
      if (microphone_callback) microphone_callback(chunk.samples.data(),
          static_cast<uint32_t>(chunk.samples.size()), 1, chunk.rate, chunk.time, microphone_data);
      lock.lock();
    }
  }
  void Deliver() {
    std::lock_guard<std::mutex> delivery(delivery_mutex);
    DeliverLocked();
  }
};

extern "C" mrec_voice_processor *mrec_voice_create(int32_t flags,
    mrec_voice_callback system, void *system_data,
    mrec_voice_callback microphone, void *microphone_data) {
  if ((flags & ~MREC_MICROPHONE_PROCESSING_MASK) != 0) return nullptr;
  try { return new mrec_voice_processor(flags, system, system_data, microphone, microphone_data); }
  catch (...) { return nullptr; }
}

extern "C" void mrec_voice_set_reference_wait(mrec_voice_processor *processor, uint32_t milliseconds) {
  if (!processor) return;
  std::lock_guard<std::mutex> lock(processor->mutex);
  processor->reference_wait_ns = std::min(milliseconds, uint32_t(1000)) * 1000000ull;
}

extern "C" void mrec_voice_system(const float *samples, uint32_t frames, uint32_t channels,
                                    double rate, uint64_t time, void *context) {
  auto *processor = static_cast<mrec_voice_processor *>(context);
  if (!processor) return;
  if (processor->flags & MREC_MICROPHONE_ECHO_CANCELLATION && Valid(samples, frames, channels, rate)) {
    {
      std::lock_guard<std::mutex> lock(processor->mutex);
      const size_t count = std::min(static_cast<size_t>(frames), kReferenceSamples);
      const size_t skipped = frames - count;
      Reference chunk{time + Duration(skipped, rate), rate, std::vector<float>(count)};
      for (size_t i = 0; i < count; ++i) {
        double sum = 0;
        for (size_t ch = 0; ch < channels; ++ch) sum += Finite(samples[(skipped + i) * channels + ch]);
        chunk.samples[i] = static_cast<float>(sum / channels);
      }
      const auto position = std::upper_bound(processor->reference.begin(), processor->reference.end(), chunk.time,
          [](uint64_t t, const Reference &r) { return t < r.time; });
      processor->reference.insert(position, std::move(chunk));
      processor->reference_samples += count;
      processor->latest_time = std::max(processor->latest_time, time + Duration(frames, rate));
      processor->Drain(false);
      while (!processor->reference.empty() &&
          (processor->reference_samples > kReferenceSamples || processor->reference.size() > 512 ||
           processor->latest_time > processor->reference.front().End() + kHistoryNs)) {
        processor->reference_samples -= processor->reference.front().samples.size();
        processor->reference.pop_front();
      }
    }
    processor->Deliver();
  }
  if (processor->system_callback) processor->system_callback(samples, frames, channels, rate, time, processor->system_data);
}

extern "C" void mrec_voice_microphone(const float *samples, uint32_t frames, uint32_t channels,
                                        double rate, uint64_t time, void *context) {
  auto *processor = static_cast<mrec_voice_processor *>(context);
  if (!processor) return;
  if (!processor->flags) {
    if (processor->microphone_callback) processor->microphone_callback(samples, frames, channels, rate, time, processor->microphone_data);
    return;
  }
  if (!Valid(samples, frames, channels, rate)) return;
  // Backpressure prevents another producer accumulating mic output while the
  // system thread delivers a slow mic callback. Callbacks remain serialized.
  std::lock_guard<std::mutex> delivery(processor->delivery_mutex);
  {
    std::lock_guard<std::mutex> lock(processor->mutex);
    const uint64_t expected = processor->next_mic_time;
    const bool gap = expected && (time > expected ? time - expected : expected - time) > 20000000;
    if (processor->rate != rate || gap) {
      processor->FlushLocked();
      processor->DestroyDSP();
      processor->Configure(rate);
    }
    if (!processor->have_mic_timeline) {
      processor->mic_origin = time;
      processor->mic_received = 0;
      processor->have_mic_timeline = true;
    }
    processor->next_mic_time = processor->mic_origin + Duration(processor->mic_received + frames, rate);
    processor->latest_time = std::max(processor->latest_time, processor->next_mic_time);
    for (size_t i = 0; i < frames; ++i) {
      if (processor->partial.empty()) processor->partial_time = processor->mic_origin + Duration(processor->mic_received + i, rate);
      double sum = 0;
      for (size_t ch = 0; ch < channels; ++ch) sum += Finite(samples[i * channels + ch]);
      processor->partial.push_back(PCM(static_cast<float>(sum / channels)));
      if (processor->partial.size() == processor->frame_size) {
        processor->pending.push_back(Frame{processor->partial_time, rate, processor->frame_size,
                                           std::move(processor->partial)});
        processor->partial.clear();
        processor->partial.reserve(processor->frame_size);
      }
    }
    processor->mic_received += frames;
    processor->Drain(false);
  }
  processor->DeliverLocked();
}

extern "C" void mrec_voice_reset_microphone(mrec_voice_processor *processor) {
  if (!processor) return;
  std::lock_guard<std::mutex> delivery(processor->delivery_mutex);
  {
    std::lock_guard<std::mutex> lock(processor->mutex);
    processor->FlushLocked();
    processor->DestroyDSP();
  }
  processor->DeliverLocked();
}
extern "C" void mrec_voice_flush(mrec_voice_processor *processor) {
  mrec_voice_reset_microphone(processor);
}
extern "C" void mrec_voice_destroy(mrec_voice_processor *processor) { delete processor; }
