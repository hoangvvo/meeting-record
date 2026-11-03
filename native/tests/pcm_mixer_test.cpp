#include "../include/meeting-record-mix.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {
using Mixer = std::unique_ptr<mrec_pcm_mixer, decltype(&mrec_pcm_mixer_destroy)>;
Mixer Create(uint32_t rate, uint32_t sources = 1) {
  Mixer mixer(mrec_pcm_mixer_create(rate, sources), mrec_pcm_mixer_destroy);
  assert(mixer);
  return mixer;
}
void Write(Mixer &mixer, const std::vector<float> &pcm, double rate,
           uint64_t time = 0, uint32_t channels = 1, uint32_t source = 0) {
  assert(mrec_pcm_mixer_write(mixer.get(), source, pcm.data(), uint32_t(pcm.size() / channels),
                            channels, rate, time) == 0);
}
std::vector<int16_t> Drain(Mixer &mixer) {
  std::vector<int16_t> result;
  const uint8_t *pcm;
  size_t bytes;
  float level;
  int status;
  while ((status = mrec_pcm_mixer_read(mixer.get(), &pcm, &bytes, &level)) == 1) {
    assert(level >= 0 && level <= 1 && bytes % 2 == 0);
    for (size_t i = 0; i < bytes; i += 2) result.push_back(int16_t(uint16_t(pcm[i]) | uint16_t(pcm[i + 1]) << 8));
  }
  assert(status == 0 && pcm == nullptr && bytes == 0 && level == 0);
  return result;
}
std::vector<int16_t> Convert(const std::vector<float> &input, double rate,
                             uint32_t output_rate, size_t split, uint32_t channels = 1) {
  auto mixer = Create(output_rate);
  for (size_t offset = 0; offset < input.size(); offset += split) {
    const auto end = (std::min)(input.size(), offset + split);
    Write(mixer, {input.begin() + offset, input.begin() + end}, rate,
          uint64_t(std::llround(double(offset / channels) * 1e9 / rate)), channels);
  }
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  auto result = Drain(mixer);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  assert(Drain(mixer).empty());
  return result;
}
void Resampling() {
  std::vector<float> stereo;
  for (size_t i = 0; i < 1001; ++i) {
    stereo.push_back(float(std::sin(double(i) * 0.19)));
    stereo.push_back(-0.25f);
  }
  for (const auto rate : {8000., 44100., 48000., 96000.}) {
    const auto whole = Convert(stereo, rate, 16000, stereo.size(), 2);
    const auto split = Convert(stereo, rate, 16000, 14, 2);
    assert(whole == split);
    assert(split.size() == size_t(std::ceil(1001 * 16000 / rate)));
  }
  assert(Convert(std::vector<float>(80, 0.25f), 8000, 24000, 7) == std::vector<int16_t>(240, 8192));
  assert(Convert({0.75f, 0.25f, 0.75f, 0.25f, 0.75f, 0.25f}, 4, 2, 6, 2) ==
      std::vector<int16_t>({16384, 16384}));
}
void Aliases() {
  constexpr double pi = 3.14159265358979323846;
  for (const auto rate : {44100., 48000., 96000.}) {
    for (const auto frequency : {1000., 10000.}) {
      std::vector<float> input(size_t(rate), 0);
      for (size_t i = 0; i < input.size(); ++i) input[i] = float(std::sin(2 * pi * frequency * double(i) / rate));
      const auto output = Convert(input, rate, 16000, 257);
      assert(output.size() == 16000);
      const auto alias = frequency < 8000 ? frequency : 16000 - frequency;
      double real = 0, imaginary = 0;
      for (size_t i = 200; i < output.size() - 200; ++i) {
        const auto angle = 2 * pi * alias * double(i) / 16000;
        real += double(output[i]) / 32768 * std::cos(angle);
        imaginary += double(output[i]) / 32768 * std::sin(angle);
      }
      const auto amplitude = 2 * std::hypot(real, imaginary) / double(output.size() - 400);
      assert(frequency < 8000 ? std::abs(amplitude - 1) < 0.001 : amplitude < 0.001);
    }
  }
}
void Boundaries() {
  for (const auto rate : {44100., 48000., 96000.}) {
    auto mixer = Create(16000);
    Write(mixer, std::vector<float>(size_t(rate / 10), 0.25f), rate);
    Write(mixer, std::vector<float>(size_t(rate / 10), 0.5f), rate, 500000000);
    assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
    const auto output = Drain(mixer);
    assert(output.size() == 9600);
    for (size_t i = 0; i < output.size(); ++i) assert(output[i] == (i < 1600 ? 8192 : i < 8000 ? 0 : 16384));
  }
  auto mixer = Create(16000);
  Write(mixer, std::vector<float>(301, 0.25f), 48000);
  Write(mixer, std::vector<float>(160, 0.5f), 16000, 6270833);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  auto result = Drain(mixer);
  assert(result.size() == 261);
  for (size_t i = 0; i < result.size(); ++i) assert(result[i] == (i < 101 ? 8192 : 16384));
  assert(mrec_pcm_mixer_restart(mixer.get()) == 0);
  Write(mixer, std::vector<float>(160, 0.75f), 16000, 60000000000);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  assert(Drain(mixer) == std::vector<int16_t>(160, 24575));
}
void MixAndMute() {
  auto mixer = Create(24000, 2);
  Write(mixer, std::vector<float>(240, 0.5f), 24000, 0, 1, 1);
  Write(mixer, std::vector<float>(480, 0.25f), 48000);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  assert(mrec_pcm_mixer_set_muted(mixer.get(), 1, 1) == 0);
  assert(mrec_pcm_mixer_restart(mixer.get()) == 0);
  Write(mixer, std::vector<float>(960, 0.25f), 48000, 60000000000);
  Write(mixer, std::vector<float>(480, 0.5f), 24000, 60000000000, 1, 1);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  const auto result = Drain(mixer);
  assert(result.size() == 720);
  for (size_t i = 0; i < result.size(); ++i) {
    const auto expected = std::round((i < 240 ? 0.75 : 0.25) / std::sqrt(2.) * 32767);
    assert(std::abs(result[i] - expected) <= 1);
  }
}
void ClockAndSilence() {
  auto mixer = Create(100);
  for (const auto time : {uint64_t(0), uint64_t(60000000000)})
    Write(mixer, std::vector<float>(10, 0.25f), 100, time);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  assert(Drain(mixer).size() == 20);
  mixer = Create(100, 2);
  Write(mixer, std::vector<float>(10, 0), 100, UINT64_MAX - 100000000);
  Write(mixer, std::vector<float>(10, 0.5f), 100, UINT64_MAX - 150000000, 1, 1);
  assert(mrec_pcm_mixer_finish_segment(mixer.get()) == 0);
  const auto output = Drain(mixer);
  assert(output.size() == 10);
  for (size_t i = 0; i < output.size(); ++i) assert(i < 5 ? output[i] > 0 : output[i] == 0);
}
void EncodingAndValidation() {
  assert(!mrec_pcm_mixer_create(0, 1));
  assert(!mrec_pcm_mixer_create(16000, 0));
  assert(!mrec_pcm_mixer_create(16000, 3));
  auto mixer = Create(10);
  const std::vector<float> samples = {-2, 2, std::numeric_limits<float>::quiet_NaN()};
  Write(mixer, samples, 10);
  const uint8_t *pcm;
  size_t bytes;
  float level;
  assert(mrec_pcm_mixer_read(mixer.get(), &pcm, &bytes, &level) == 1);
  assert(bytes == 2 && pcm[0] == 0 && pcm[1] == 128 && level == 1);
  assert(mrec_pcm_mixer_read(mixer.get(), &pcm, &bytes, &level) == 1);
  assert(bytes == 2 && pcm[0] == 255 && pcm[1] == 127 && std::abs(level - 32767.f / 32768) < 1e-6);
  assert(Drain(mixer) == std::vector<int16_t>({0}));
  assert(samples[0] == -2 && samples[1] == 2 && std::isnan(samples[2]));
  assert(mrec_pcm_mixer_write(mixer.get(), 5, samples.data(), 3, 1, 10, 0) == -1);
  assert(mrec_pcm_mixer_write(mixer.get(), 0, nullptr, 3, 1, 10, 0) == -1);
  assert(mrec_pcm_mixer_write(mixer.get(), 0, nullptr, 0, 1, 10, 0) == 0);
  assert(mrec_pcm_mixer_write(mixer.get(), 0, samples.data(), 3, 0, 10, 0) == -1);
  assert(mrec_pcm_mixer_write(mixer.get(), 0, samples.data(), 3, 1, 0, 0) == -1);
  assert(mrec_pcm_mixer_write(mixer.get(), 0, samples.data(), 3, 1, 1e-200, 0) == -1);
  assert(mrec_pcm_mixer_write(mixer.get(), 0, samples.data(), 3, 1, 1e200, 0) == -1);
  assert(mrec_pcm_mixer_read(mixer.get(), nullptr, &bytes, &level) == -1);
}
} // namespace

int main() {
  Resampling();
  Aliases();
  Boundaries();
  MixAndMute();
  ClockAndSilence();
  EncodingAndValidation();
  puts("shared PCM mixer tests passed");
}
