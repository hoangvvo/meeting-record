#pragma once

#include "Error.h"
#include "Process.h"
#include <atomic>
#include <pulse/pulseaudio.h>
#include <thread>
#include <vector>

namespace meeting_record::linux_backend {
struct AudioStream {
  uint32_t index = 0, device = 0, client = PA_INVALID_INDEX, pid = 0;
  bool input = false, active = false;
  std::string name;
};
struct AudioDevice {
  uint32_t index = 0, monitor = PA_INVALID_INDEX;
  std::string name, description;
};
struct Snapshot {
  std::vector<AudioStream> streams;
  std::vector<AudioDevice> sinks, sources;
  std::string default_source;
};

// Each connection belongs to exactly one worker. No PulseAudio callback blocks
// waiting for another callback. All server operations have a bounded deadline.
class Pulse {
public:
  explicit Pulse(const std::atomic<bool> *cancel = nullptr) : cancel_(cancel) {}
  Pulse(const Pulse &) = delete;
  ~Pulse() { Close(); }
  bool Open() {
    Close();
    loop = pa_mainloop_new();
    if (!loop)
      return false;
    pa_proplist *props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "meeting-record");
    const auto pid = std::to_string(getpid());
    pa_proplist_sets(props, PA_PROP_APPLICATION_PROCESS_ID, pid.c_str());
    context = pa_context_new_with_proplist(pa_mainloop_get_api(loop),
                                           "meeting-record", props);
    pa_proplist_free(props);
    if (!context || pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN,
                                       nullptr) < 0)
      return false;
    const auto deadline = NowNs() + 2000000000ull;
    while (pa_context_get_state(context) != PA_CONTEXT_READY) {
      if (!Good() || NowNs() >= deadline)
        return false;
      Iterate();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pa_context_set_subscribe_callback(
        context,
        [](pa_context *, pa_subscription_event_type_t, uint32_t, void *) {},
        nullptr);
    bool subscribed = false;
    const auto mask = pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SERVER |
                                             PA_SUBSCRIPTION_MASK_SINK |
                                             PA_SUBSCRIPTION_MASK_SOURCE);
    return Wait(pa_context_subscribe(
               context, mask,
               [](pa_context *, int success, void *data) {
                 *static_cast<bool *>(data) = success != 0;
               },
               &subscribed)) &&
           subscribed;
  }
  void Close() {
    if (context) {
      pa_context_disconnect(context);
      pa_context_unref(context);
      context = nullptr;
    }
    if (loop)
      pa_mainloop_free(loop);
    loop = nullptr;
  }
  bool Good() const {
    return !(cancel_ && cancel_->load()) && context &&
           PA_CONTEXT_IS_GOOD(pa_context_get_state(context));
  }
  void Iterate() {
    if (loop)
      pa_mainloop_iterate(loop, 0, nullptr);
  }
  bool Wait(pa_operation *operation) {
    if (!operation)
      return false;
    const auto deadline = NowNs() + 2000000000ull;
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING &&
           Good() && NowNs() < deadline) {
      Iterate();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool done =
        Good() && pa_operation_get_state(operation) == PA_OPERATION_DONE;
    if (!done)
      pa_operation_cancel(operation);
    pa_operation_unref(operation);
    return done;
  }
  bool Read(Snapshot &out) {
    out = {};
    bool failed = false;
    struct ReadState {
      Snapshot *out;
      bool *failed;
    } state{&out, &failed};
    if (!Wait(pa_context_get_sink_input_info_list(
            context,
            [](pa_context *, const pa_sink_input_info *info, int end,
               void *data) {
              auto &s = *static_cast<ReadState *>(data);
              if (end < 0)
                *s.failed = true;
              if (!info)
                return;
              s.out->streams.push_back(
                  {info->index, info->sink, info->client, Pid(info->proplist),
                   false, !info->corked, info->name ? info->name : ""});
            },
            &state)))
      return false;
    if (!Wait(pa_context_get_source_output_info_list(
            context,
            [](pa_context *, const pa_source_output_info *info, int end,
               void *data) {
              auto &s = *static_cast<ReadState *>(data);
              if (end < 0)
                *s.failed = true;
              if (!info)
                return;
              s.out->streams.push_back(
                  {info->index, info->source, info->client, Pid(info->proplist),
                   true, !info->corked, info->name ? info->name : ""});
            },
            &state)))
      return false;
    if (!Wait(pa_context_get_client_info_list(
            context,
            [](pa_context *, const pa_client_info *info, int end, void *data) {
              auto &s = *static_cast<ReadState *>(data);
              if (end < 0)
                *s.failed = true;
              if (!info)
                return;
              for (auto &stream : s.out->streams)
                if (!stream.pid && stream.client == info->index)
                  stream.pid = Pid(info->proplist);
            },
            &state)))
      return false;
    if (!Wait(pa_context_get_sink_info_list(
            context,
            [](pa_context *, const pa_sink_info *info, int end, void *data) {
              auto &s = *static_cast<ReadState *>(data);
              if (end < 0)
                *s.failed = true;
              if (info)
                s.out->sinks.push_back(
                    {info->index, info->monitor_source,
                     info->name ? info->name : "",
                     info->description ? info->description : ""});
            },
            &state)))
      return false;
    if (!Wait(pa_context_get_source_info_list(
            context,
            [](pa_context *, const pa_source_info *info, int end, void *data) {
              auto &s = *static_cast<ReadState *>(data);
              if (end < 0)
                *s.failed = true;
              // Exclude the built-in output monitors by their actual source ID;
              // remapped and virtual inputs have their own eligible source IDs.
              if (info && std::none_of(s.out->sinks.begin(), s.out->sinks.end(),
                                       [&](const auto &sink) {
                                         return sink.monitor == info->index;
                                       }))
                s.out->sources.push_back(
                    {info->index, PA_INVALID_INDEX,
                     info->name ? info->name : "",
                     info->description ? info->description : ""});
            },
            &state)))
      return false;
    if (!Wait(pa_context_get_server_info(
            context,
            [](pa_context *, const pa_server_info *info, void *data) {
              auto &s = *static_cast<ReadState *>(data);
              if (!info)
                *s.failed = true;
              else
                s.out->default_source =
                    info->default_source_name ? info->default_source_name : "";
            },
            &state)))
      return false;
    return !failed;
  }
  bool DefaultSource(std::string &name) {
    bool received = false;
    struct State {
      std::string *name;
      bool *received;
    } state{&name, &received};
    return Wait(pa_context_get_server_info(
               context,
               [](pa_context *, const pa_server_info *info, void *data) {
                 auto &state = *static_cast<State *>(data);
                 if (!info)
                   return;
                 *state.name =
                     info->default_source_name ? info->default_source_name : "";
                 *state.received = true;
               },
               &state)) &&
           received;
  }
  bool Ready(pa_stream *stream) {
    const auto deadline = NowNs() + 2000000000ull;
    while (pa_stream_get_state(stream) != PA_STREAM_READY) {
      if (!Good() || !PA_STREAM_IS_GOOD(pa_stream_get_state(stream)) ||
          NowNs() >= deadline)
        return false;
      Iterate();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  std::string Error() const {
    return context ? pa_strerror(pa_context_errno(context))
                   : "audio server unavailable";
  }
  mrec_status FailureStatus() const {
    return context && pa_context_errno(context) == PA_ERR_ACCESS
               ? MREC_ERR_PERMISSION
               : MREC_ERR_DEVICE_FAILED;
  }
  pa_mainloop *loop = nullptr;
  pa_context *context = nullptr;

private:
  const std::atomic<bool> *cancel_;
  static uint32_t Pid(pa_proplist *props) {
    const auto value = pa_proplist_gets(props, PA_PROP_APPLICATION_PROCESS_ID);
    if (!value || !*value)
      return 0;
    char *end = nullptr;
    const auto pid = std::strtoul(value, &end, 10);
    return end && !*end && pid > 0 && pid <= UINT32_MAX ? uint32_t(pid) : 0;
  }
};
} // namespace meeting_record::linux_backend
