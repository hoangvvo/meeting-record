#include "../include/meeting-record-mix.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace {
constexpr size_t kPhases = 64;
constexpr double kPi = 3.14159265358979323846;

class Downsampler {
public:
  explicit Downsampler(double step)
      : step_(step), radius_(static_cast<size_t>((std::min)(4096.0, std::ceil(24 * step)))),
        width_(2 * radius_ + 1), kernel_((kPhases + 1) * width_) {
    const double cutoff = 0.45 / step;
    for (size_t phase = 0; phase <= kPhases; ++phase) {
      double sum = 0;
      for (size_t tap = 0; tap < width_; ++tap) {
        const double distance = double(tap) - double(radius_) - double(phase) / kPhases;
        const double angle = 2 * kPi * cutoff * distance;
        const double theta = kPi * distance / double(radius_);
        const double window = std::abs(distance) > double(radius_) ? 0 :
            0.42 + 0.5 * std::cos(theta) + 0.08 * std::cos(2 * theta);
        const double weight = (angle == 0 ? 1 : std::sin(angle) / angle) * window;
        kernel_[phase * width_ + tap] = weight;
        sum += weight;
      }
      for (size_t tap = 0; tap < width_; ++tap) kernel_[phase * width_ + tap] /= sum;
    }
  }

  double Pending() const { return (std::max)(0.0, double(received_) / step_ - double(written_)); }
  std::vector<float> Resample(const std::vector<float> &input) {
    if (input.empty()) return {};
    if (!received_) first_ = input.front();
    last_ = input.back();
    samples_.insert(samples_.end(), input.begin(), input.end());
    received_ += input.size();
    return Drain(false);
  }
  std::vector<float> Flush() {
    auto result = Drain(true);
    samples_.clear();
    offset_ = received_ = written_ = 0;
    return result;
  }

private:
  std::vector<float> Drain(bool final) {
    std::vector<float> output;
    output.reserve(static_cast<size_t>(std::ceil(Pending())));
    while (double(written_) * step_ < double(received_) - 1e-9) {
      const double position = double(written_) * step_;
      const auto center = static_cast<size_t>(std::floor(position));
      if (!final && center + radius_ >= received_) break;
      const double fractional = (position - double(center)) * kPhases;
      const auto phase = (std::min)(kPhases - 1, static_cast<size_t>(std::floor(fractional)));
      const double blend = fractional - double(phase);
      double value = 0;
      for (size_t tap = 0; tap < width_; ++tap) {
        const int64_t index = int64_t(center) - int64_t(radius_) + int64_t(tap);
        const float sample = index < 0 ? first_ : size_t(index) >= received_ ? last_ :
            samples_[size_t(index) - offset_];
        const double weight = kernel_[phase * width_ + tap];
        const double next = kernel_[(phase + 1) * width_ + tap];
        value += sample * (weight + (next - weight) * blend);
      }
      output.push_back(static_cast<float>(value));
      ++written_;
    }
    const auto next = static_cast<size_t>(std::floor(double(written_) * step_));
    const size_t keep = next > radius_ ? next - radius_ : 0;
    const size_t drop = (std::min)(samples_.size(), keep - offset_);
    samples_.erase(samples_.begin(), samples_.begin() + drop);
    offset_ += drop;
    return output;
  }
  double step_;
  size_t radius_, width_;
  std::vector<double> kernel_;
  std::vector<float> samples_;
  size_t offset_ = 0, received_ = 0, written_ = 0;
  float first_ = 0, last_ = 0;
};

class MonoResampler {
public:
  explicit MonoResampler(uint32_t output_rate) : output_rate_(output_rate) {}
  void Reset() {
    sample_rate_ = 0;
    step_ = 1;
    previous_ = 0;
    position_ = 0;
    filter_.reset();
  }
  double Pending() const {
    return filter_ ? filter_->Pending() : (std::max)(0.0, -position_ / step_);
  }
  std::vector<float> Resample(const float *samples, uint32_t frames, uint32_t channels, double rate) {
    std::vector<float> output;
    if (sample_rate_ != rate) {
      output = Flush();
      Reset();
      sample_rate_ = rate;
      step_ = rate / output_rate_;
      if (step_ > 1) filter_ = std::make_unique<Downsampler>(step_);
    }
    std::vector<float> input(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
      double sum = 0;
      for (size_t channel = 0; channel < channels; ++channel) {
        const float sample = samples[frame * channels + channel];
        if (std::isfinite(sample)) sum += sample;
      }
      input[frame] = static_cast<float>(sum / channels);
    }
    if (step_ == 1) output.insert(output.end(), input.begin(), input.end());
    else if (filter_) {
      auto converted = filter_->Resample(input);
      output.insert(output.end(), converted.begin(), converted.end());
    } else if (!input.empty()) {
      const double last = double(input.size()) - 1;
      while (position_ < last) {
        const auto index = static_cast<int64_t>(std::floor(position_));
        const double from = index < 0 ? previous_ : input[size_t(index)];
        const double to = input[size_t(index + 1)];
        output.push_back(static_cast<float>(from + (to - from) * (position_ - double(index))));
        position_ += step_;
      }
      position_ -= double(input.size());
      previous_ = input.back();
    }
    return output;
  }
  std::vector<float> Flush() {
    if (filter_) return filter_->Flush();
    const auto count = static_cast<size_t>((std::max)(0.0, std::ceil(-position_ / step_ - 1e-9)));
    position_ = 0;
    return std::vector<float>(count, previous_);
  }
private:
  uint32_t output_rate_;
  double sample_rate_ = 0, step_ = 1, position_ = 0;
  float previous_ = 0;
  std::unique_ptr<Downsampler> filter_;
};

struct Input {
  explicit Input(uint32_t rate) : resampler(rate) {}
  MonoResampler resampler;
  std::optional<int64_t> cursor;
  bool muted = false;
};
} // namespace

struct mrec_pcm_mixer {
  uint32_t sample_rate;
  int64_t block_frames;
  std::vector<Input> inputs;
  double gain;
  std::map<int64_t, std::vector<float>> blocks;
  std::optional<uint64_t> origin;
  int64_t origin_frame = 0, emitted = 0, end = 0, committed = 0;
  std::vector<uint8_t> pcm;

  mrec_pcm_mixer(uint32_t rate, uint32_t sources)
      : sample_rate(rate), block_frames((std::max)(int64_t(1), int64_t(std::round(double(rate) / 10)))),
        gain(1 / std::sqrt(double(sources))) {
    inputs.reserve(sources);
    for (uint32_t i = 0; i < sources; ++i) inputs.emplace_back(rate);
  }
  void Append(Input &input, const std::vector<float> &samples) {
    if (!input.cursor) return;
    const int64_t start = *input.cursor;
    input.cursor = start + int64_t(samples.size());
    end = (std::max)(end, *input.cursor);
    if (input.muted) return;
    for (size_t i = size_t((std::max)(int64_t(0), emitted - start)); i < samples.size(); ++i) {
      const int64_t frame = start + int64_t(i);
      auto [entry, inserted] = blocks.try_emplace(frame / block_frames);
      if (inserted) entry->second.resize(size_t(block_frames));
      auto &sample = entry->second[size_t(frame % block_frames)];
      sample = static_cast<float>(sample + double(samples[i]) * gain);
    }
  }
  void Finish() {
    for (auto &input : inputs) Append(input, input.resampler.Flush());
    committed = end;
  }
  void Restart() {
    origin.reset();
    origin_frame = end;
    for (auto &input : inputs) { input.cursor.reset(); input.resampler.Reset(); }
  }
  bool Write(uint32_t source, const float *samples, uint32_t frames, uint32_t channels,
             double rate, uint64_t time) {
    if (!origin) origin = time;
    // Subtract unsigned clocks before conversion, preserving small deltas near UINT64_MAX.
    const double elapsed = time >= *origin ? double(time - *origin) : -double(*origin - time);
    const double offset = std::floor(elapsed * sample_rate / 1e9 + 0.5);
    if (std::abs(offset) > double(std::numeric_limits<int64_t>::max() / 2)) return false;
    int64_t position = origin_frame + int64_t(offset);
    if (position > end + sample_rate) {
      Finish();
      Restart();
      origin = time;
      position = origin_frame;
    }
    auto &input = inputs[source];
    if (input.cursor && std::abs(double(position - *input.cursor) - input.resampler.Pending()) >
        double(sample_rate) / 50) {
      Append(input, input.resampler.Flush());
      input.resampler.Reset();
      input.cursor = position;
    }
    if (!input.cursor) input.cursor = position;
    Append(input, input.resampler.Resample(samples, frames, channels, rate));
    return true;
  }
  bool Read(float &level) {
    int64_t ready = committed;
    std::optional<int64_t> lead;
    for (const auto &input : inputs) if (input.cursor)
      lead = lead ? (std::max)(*lead, *input.cursor) : *input.cursor;
    if (lead) {
      int64_t active = *lead;
      for (const auto &input : inputs)
        if (input.cursor && double(*lead - *input.cursor) <= double(sample_rate) / 2)
          active = (std::min)(active, *input.cursor);
      ready = (std::max)(ready, active);
    }
    const int64_t until = (std::min)(ready, emitted + block_frames);
    if (until <= emitted) return false;
    pcm.resize(size_t(until - emitted) * 2);
    double squares = 0;
    for (int64_t frame = emitted; frame < until; ++frame) {
      const auto block = blocks.find(frame / block_frames);
      const float value = block == blocks.end() ? 0 : block->second[size_t(frame % block_frames)];
      const double sample = std::isfinite(value) ? (std::clamp)(value, -1.f, 1.f) : 0;
      const auto encoded = static_cast<int16_t>(std::floor(sample * (sample < 0 ? 32768 : 32767) + 0.5));
      const auto bits = static_cast<uint16_t>(encoded);
      const size_t index = size_t(frame - emitted) * 2;
      pcm[index] = uint8_t(bits & 0xff);
      pcm[index + 1] = uint8_t(bits >> 8);
      const double normalized = double(encoded) / 32768;
      squares += normalized * normalized;
    }
    auto first = blocks.begin();
    while (first != blocks.end() && (first->first + 1) * block_frames <= until) first = blocks.erase(first);
    level = static_cast<float>(std::sqrt(squares / double(until - emitted)));
    emitted = until;
    return true;
  }
};

extern "C" mrec_pcm_mixer *mrec_pcm_mixer_create(uint32_t rate, uint32_t sources) {
  if (!rate || !sources || sources > 2) return nullptr;
  try { return new mrec_pcm_mixer(rate, sources); } catch (...) { return nullptr; }
}
extern "C" void mrec_pcm_mixer_destroy(mrec_pcm_mixer *mixer) { delete mixer; }
extern "C" int32_t mrec_pcm_mixer_write(mrec_pcm_mixer *mixer, uint32_t source,
    const float *samples, uint32_t frames, uint32_t channels, double rate, uint64_t time) {
  if (!mixer || source >= mixer->inputs.size() || !channels || channels > 32 ||
      (frames && !samples) || !std::isfinite(rate) || rate < 1 ||
      rate > double(std::numeric_limits<uint32_t>::max())) return -1;
  if (!frames) return 0;
  try { return mixer->Write(source, samples, frames, channels, rate, time) ? 0 : -1; }
  catch (...) { return -2; }
}
extern "C" int32_t mrec_pcm_mixer_set_muted(mrec_pcm_mixer *mixer, uint32_t source, int32_t muted) {
  if (!mixer || source >= mixer->inputs.size()) return -1;
  mixer->inputs[source].muted = muted != 0;
  return 0;
}
extern "C" int32_t mrec_pcm_mixer_finish_segment(mrec_pcm_mixer *mixer) {
  if (!mixer) return -1;
  try { mixer->Finish(); return 0; } catch (...) { return -2; }
}
extern "C" int32_t mrec_pcm_mixer_restart(mrec_pcm_mixer *mixer) {
  if (!mixer) return -1;
  mixer->Restart();
  return 0;
}
extern "C" int32_t mrec_pcm_mixer_read(mrec_pcm_mixer *mixer, const uint8_t **pcm, size_t *bytes, float *level) {
  if (!mixer || !pcm || !bytes || !level) return -1;
  *pcm = nullptr;
  *bytes = 0;
  *level = 0;
  try {
    if (!mixer->Read(*level)) return 0;
    *pcm = mixer->pcm.data();
    *bytes = mixer->pcm.size();
    return 1;
  } catch (...) { return -2; }
}
