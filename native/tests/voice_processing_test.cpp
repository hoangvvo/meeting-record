#include "../common/voice-processing.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct Recorded {
  std::vector<float> microphone;
  std::vector<float> system;
  std::vector<uint64_t> times;
  std::vector<uint32_t> sizes;
  static void Mic(const float *data, uint32_t frames, uint32_t channels,
                  double rate, uint64_t time, void *context) {
    auto &out = *static_cast<Recorded *>(context);
    assert(channels == 1 && rate >= 8000);
    out.microphone.insert(out.microphone.end(), data, data + frames);
    out.times.push_back(time);
    out.sizes.push_back(frames);
  }
  static void System(const float *data, uint32_t frames, uint32_t channels,
                     double, uint64_t, void *context) {
    auto &out = *static_cast<Recorded *>(context);
    out.system.insert(out.system.end(), data, data + frames * channels);
  }
};
static double Energy(const std::vector<float> &samples, size_t skip = 0) {
  double result = 0;
  for (size_t i = skip; i < samples.size(); ++i) result += samples[i] * samples[i];
  return result / (samples.size() - skip);
}
static std::vector<float> Noise(size_t size, float amplitude) {
  uint32_t state = 139;
  std::vector<float> samples(size);
  for (float &sample : samples) {
    state = state * 1664525u + 1013904223u;
    sample = amplitude * (static_cast<int32_t>(state) / 2147483648.f);
  }
  return samples;
}
static Recorded Run(int flags, const std::vector<float> &system,
                    const std::vector<float> &mic, bool mic_first = false, size_t reference_delay = 0) {
  Recorded out;
  auto *processor = mrec_voice_create(flags, Recorded::System, &out, Recorded::Mic, &out);
  assert(processor);
  if (reference_delay) mrec_voice_set_reference_wait(processor, 250);
  for (size_t offset = 0; offset < mic.size(); offset += 160) {
    const auto count = static_cast<uint32_t>(std::min(size_t(160), mic.size() - offset));
    const uint64_t time = offset * 1000000000ull / 16000;
    auto microphone = [&] { mrec_voice_microphone(mic.data() + offset, count, 1, 16000, time, processor); };
    auto reference = [&] {
      if (system.empty() || offset < reference_delay) return;
      const auto position = offset - reference_delay;
      const auto reference_count = static_cast<uint32_t>(std::min(size_t(160), system.size() - position));
      mrec_voice_system(system.data() + position, reference_count, 1, 16000,
                       position * 1000000000ull / 16000, processor);
    };
    if (mic_first) { microphone(); reference(); }
    else { reference(); microphone(); }
  }
  if (!system.empty() && reference_delay) {
    for (size_t offset = (mic.size() + 159) / 160 * 160 - reference_delay;
         offset < system.size(); offset += 160) {
      const auto count = static_cast<uint32_t>(std::min(size_t(160), system.size() - offset));
      mrec_voice_system(system.data() + offset, count, 1, 16000,
                       offset * 1000000000ull / 16000, processor);
    }
  }
  mrec_voice_flush(processor);
  const auto size = out.microphone.size();
  mrec_voice_flush(processor);
  assert(out.microphone.size() == size);
  mrec_voice_destroy(processor);
  assert(out.microphone.size() == mic.size());
  assert(out.system == system);
  size_t position = 0;
  for (size_t i = 0; i < out.times.size(); ++i) {
    assert(out.times[i] == position * 1000000000ull / 16000);
    position += out.sizes[i];
  }
  return out;
}

int main() {
  for (int flags = 0; flags <= 0x700; flags += 0x100) {
    assert(mrec_microphone_configuration_valid(flags | 1));
    assert(mrec_microphone_configuration_valid(flags | 2));
    assert(mrec_microphone_source_value(flags | 2) == 2);
  }
  assert(!mrec_microphone_configuration_valid(0x100));
  assert(!mrec_microphone_configuration_valid(0x800 | 1));
  assert(!mrec_voice_create(8, nullptr, nullptr, nullptr, nullptr));

  // Odd device rates and fragmented input must preserve duration and time.
  for (double rate : {8000., 8300., 11025., 44100., 48000., 96000., 192000.}) {
    Recorded out;
    auto *processor = mrec_voice_create(0x700, nullptr, nullptr, Recorded::Mic, &out);
    auto input = Noise(1997, 0.1f);
    for (size_t offset = 0; offset < input.size(); offset += 13) {
      const auto count = static_cast<uint32_t>(std::min(size_t(13), input.size() - offset));
      const uint64_t time = 5000000000ull + static_cast<uint64_t>(std::llround(offset * 1e9 / rate));
      mrec_voice_microphone(input.data() + offset, count, 1, rate, time, processor);
    }
    mrec_voice_flush(processor);
    mrec_voice_destroy(processor);
    assert(out.microphone.size() == input.size());
    size_t position = 0;
    for (size_t i = 0; i < out.times.size(); ++i) {
      assert(out.times[i] == 5000000000ull + static_cast<uint64_t>(std::llround(position * 1e9 / rate)));
      position += out.sizes[i];
    }
  }

  struct Blocking {
    Recorded out;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    static void Mic(const float *samples, uint32_t frames, uint32_t channels,
                    double rate, uint64_t time, void *context) {
      auto &state = *static_cast<Blocking *>(context);
      {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.entered = true;
        state.changed.notify_all();
        assert(state.changed.wait_for(lock, std::chrono::seconds(2), [&] { return state.released; }));
      }
      Recorded::Mic(samples, frames, channels, rate, time, &state.out);
    }
  } blocked;
  auto *backpressure = mrec_voice_create(0x100, nullptr, nullptr, Blocking::Mic, &blocked);
  const auto packet = Noise(256, 0.1f);
  mrec_voice_microphone(packet.data(), 256, 1, 16000, 0, backpressure);
  std::thread system([&] { mrec_voice_system(packet.data(), 256, 1, 16000, 0, backpressure); });
  {
    std::unique_lock<std::mutex> lock(blocked.mutex);
    assert(blocked.changed.wait_for(lock, std::chrono::seconds(2), [&] { return blocked.entered; }));
  }
  std::atomic<bool> attempted{false}, returned{false};
  std::thread mic([&] {
    attempted = true;
    mrec_voice_microphone(packet.data(), 256, 1, 16000, 16000000, backpressure);
    returned = true;
  });
  while (!attempted) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(!returned);
  {
    std::lock_guard<std::mutex> lock(blocked.mutex);
    blocked.released = true;
    blocked.changed.notify_all();
  }
  system.join(); mic.join();
  mrec_voice_flush(backpressure);
  mrec_voice_destroy(backpressure);
  assert(blocked.out.microphone.size() == 512);

  const auto raw = Noise(1637, 0.2f);
  assert(Run(0, raw, raw).microphone == raw);
  for (int flags = 0x100; flags <= 0x700; flags += 0x100) {
    const auto result = Run(flags, {}, raw);
    assert(std::all_of(result.microphone.begin(), result.microphone.end(),
                      [](float sample) { return std::isfinite(sample) && std::abs(sample) <= 1; }));
  }

  auto far = Noise(16000 * 8 + 37, 0.35f);
  std::vector<float> echo(far.size());
  for (size_t i = 80; i < far.size(); ++i) echo[i] = 0.6f * far[i - 80];
  const auto cancelled = Run(MREC_MICROPHONE_ECHO_CANCELLATION, far, echo);
  const double ratio = Energy(cancelled.microphone, 16000 * 5) / Energy(echo, 16000 * 5);
  std::printf("echo residual %.2f dB\n", 10 * std::log10(ratio));
  assert(ratio < 0.01);
  const auto reordered = Run(MREC_MICROPHONE_ECHO_CANCELLATION, far, echo, true);
  assert(reordered.microphone == cancelled.microphone);
  const auto buffered = Run(MREC_MICROPHONE_ECHO_CANCELLATION, far, echo, true, 3200);
  assert(buffered.microphone == cancelled.microphone);

  const auto noise = Noise(16000 * 8, 0.08f);
  const auto suppressed = Run(MREC_MICROPHONE_NOISE_SUPPRESSION, {}, noise);
  const double noise_ratio = Energy(suppressed.microphone, 16000 * 5) / Energy(noise, 16000 * 5);
  std::printf("noise residual %.2f dB\n", 10 * std::log10(noise_ratio));
  assert(noise_ratio < 0.5);

  std::vector<float> quiet(16000 * 12);
  for (size_t i = 0; i < quiet.size(); ++i) quiet[i] = 0.01f * std::sin(i * 0.2f);
  const auto gained = Run(MREC_MICROPHONE_AUTO_GAIN_CONTROL, {}, quiet);
  const double gain = Energy(gained.microphone, 16000 * 9) / Energy(quiet, 16000 * 9);
  std::printf("automatic gain %.2f dB\n", 10 * std::log10(gain));
  assert(gain > 2);
  std::puts("voice processing tests passed");
}
