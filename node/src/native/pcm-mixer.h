#pragma once
#include <napi.h>
#include "meeting-record-mix.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

class NativePcmMixer : public Napi::ObjectWrap<NativePcmMixer> {
public:
  static void Register(Napi::Env env, Napi::Object exports) {
    exports.Set("PcmMixer", DefineClass(env, "PcmMixer", {
        InstanceMethod("write", &NativePcmMixer::Write),
        InstanceMethod("read", &NativePcmMixer::Read),
        InstanceMethod("setMuted", &NativePcmMixer::SetMuted),
        InstanceMethod("finishSegment", &NativePcmMixer::Finish),
        InstanceMethod("restart", &NativePcmMixer::Restart),
        InstanceMethod("close", &NativePcmMixer::Close)}));
  }
  explicit NativePcmMixer(const Napi::CallbackInfo &info) : Napi::ObjectWrap<NativePcmMixer>(info) {
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
      Napi::TypeError::New(info.Env(), "sample rate and source count are required").ThrowAsJavaScriptException();
      return;
    }
    const double rate = info[0].As<Napi::Number>().DoubleValue();
    const double sources = info[1].As<Napi::Number>().DoubleValue();
    if (!std::isfinite(rate) || rate < 1 || rate > std::numeric_limits<uint32_t>::max() ||
        std::floor(rate) != rate || (sources != 1 && sources != 2)) {
      Napi::RangeError::New(info.Env(), "invalid mixed audio configuration").ThrowAsJavaScriptException();
      return;
    }
    mixer_ = mrec_pcm_mixer_create(static_cast<uint32_t>(rate), static_cast<uint32_t>(sources));
    if (!mixer_) Napi::Error::New(info.Env(), "could not initialize audio mixer").ThrowAsJavaScriptException();
  }
  ~NativePcmMixer() { mrec_pcm_mixer_destroy(mixer_); }

private:
  bool Check(Napi::Env env, int32_t status = 0) const {
    if (mixer_ && status >= 0) return true;
    Napi::Error::New(env, mixer_ ? "audio mixer operation failed" : "audio mixer is closed").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Value Write(const Napi::CallbackInfo &info) {
    const auto env = info.Env();
    if (!Check(env)) return env.Undefined();
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsObject()) {
      Napi::TypeError::New(env, "source and audio chunk are required").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const auto chunk = info[1].As<Napi::Object>();
    const auto pcm = chunk.Get("pcm"), channels = chunk.Get("channels"), rate = chunk.Get("sampleRate"), time = chunk.Get("hostTimeNs");
    if (!pcm.IsBuffer() || !channels.IsNumber() || !rate.IsNumber() || !time.IsBigInt()) {
      Napi::TypeError::New(env, "invalid audio chunk").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const auto data = pcm.As<Napi::Buffer<uint8_t>>();
    const uint32_t count = channels.As<Napi::Number>().Uint32Value();
    bool lossless = false;
    const uint64_t host_time = time.As<Napi::BigInt>().Uint64Value(&lossless);
    const size_t frame_bytes = size_t(count) * sizeof(float);
    if (!lossless || !count || count > 32 || data.Length() % frame_bytes ||
        data.Length() / frame_bytes > std::numeric_limits<uint32_t>::max()) {
      Napi::TypeError::New(env, "invalid audio chunk layout or timestamp").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const auto *samples = reinterpret_cast<const float *>(data.Data());
    std::vector<float> aligned;
    if (reinterpret_cast<uintptr_t>(data.Data()) % alignof(float)) {
      aligned.resize(data.Length() / sizeof(float));
      std::memcpy(aligned.data(), data.Data(), data.Length());
      samples = aligned.data();
    }
    Check(env, mrec_pcm_mixer_write(mixer_, info[0].As<Napi::Number>().Uint32Value(), samples,
        static_cast<uint32_t>(data.Length() / frame_bytes), count,
        rate.As<Napi::Number>().DoubleValue(), host_time));
    return env.Undefined();
  }
  Napi::Value Read(const Napi::CallbackInfo &info) {
    const auto env = info.Env();
    if (!Check(env)) return env.Undefined();
    const uint8_t *pcm = nullptr;
    size_t bytes = 0;
    float level = 0;
    const auto status = mrec_pcm_mixer_read(mixer_, &pcm, &bytes, &level);
    if (!Check(env, status)) return env.Undefined();
    if (!status) return env.Null();
    auto result = Napi::Object::New(env);
    result.Set("pcm", Napi::Buffer<uint8_t>::Copy(env, pcm, bytes));
    result.Set("level", level);
    return result;
  }
  Napi::Value SetMuted(const Napi::CallbackInfo &info) {
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBoolean()) {
      Napi::TypeError::New(info.Env(), "source and mute state are required").ThrowAsJavaScriptException();
    } else if (Check(info.Env())) {
      Check(info.Env(), mrec_pcm_mixer_set_muted(mixer_, info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Boolean>().Value()));
    }
    return info.Env().Undefined();
  }
  Napi::Value Finish(const Napi::CallbackInfo &info) {
    if (Check(info.Env())) Check(info.Env(), mrec_pcm_mixer_finish_segment(mixer_));
    return info.Env().Undefined();
  }
  Napi::Value Restart(const Napi::CallbackInfo &info) {
    if (Check(info.Env())) Check(info.Env(), mrec_pcm_mixer_restart(mixer_));
    return info.Env().Undefined();
  }
  Napi::Value Close(const Napi::CallbackInfo &info) {
    mrec_pcm_mixer_destroy(mixer_);
    mixer_ = nullptr;
    return info.Env().Undefined();
  }
  mrec_pcm_mixer *mixer_ = nullptr;
};
