#include "tracks.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

struct Events {
  std::atomic<bool> in_callback{false};
  int availability[2] = {0, 0};
  uint64_t timestamp = 0;
  size_t count = 0;
};

void Record(const mrec_track_event *event, void *context) {
  auto &events = *static_cast<Events *>(context);
  assert(!events.in_callback.exchange(true));
  const int track = static_cast<int>(event->track);
  assert(track == 0 || track == 1);
  const int availability = event->type == MREC_TRACK_RESUMED ? 0
                           : event->type == MREC_TRACK_DEGRADED ? 1 : 2;
  assert(availability != events.availability[track]);
  assert(event->host_time_ns >= events.timestamp);
  events.availability[track] = availability;
  events.timestamp = event->host_time_ns;
  events.count++;
  std::this_thread::yield();
  events.in_callback = false;
}

void ConcurrentSourcesPublishSerialTransitions() {
  Events events;
  const auto session = mrec_tracks_open(Record, &events);
  assert(mrec_track_started(session, MREC_TRACK_SYSTEM_AUDIO, 3, nullptr));
  assert(mrec_track_started(session, MREC_TRACK_MICROPHONE, 1, nullptr));
  std::vector<std::thread> workers;
  for (uint32_t source = 0; source < 4; source++) {
    workers.emplace_back([session, source] {
      const auto track = source == 3 ? MREC_TRACK_MICROPHONE : MREC_TRACK_SYSTEM_AUDIO;
      const auto index = source == 3 ? 0 : source;
      for (int cycle = 0; cycle < 100; cycle++) {
        mrec_track_interrupted(session, track, index, MREC_TRACK_REASON_DEVICE_REMOVED, "lost");
        std::this_thread::yield();
        mrec_track_resumed(session, track, index, nullptr);
      }
    });
  }
  for (auto &worker : workers) worker.join();
  mrec_tracks_close();
  assert(events.count > 0);
  assert(events.availability[0] == 0 && events.availability[1] == 0);
}

struct BlockedCallback {
  std::promise<void> entered;
  std::shared_future<void> release;
  std::atomic<unsigned> calls{0};
};

void Block(const mrec_track_event *, void *context) {
  auto &callback = *static_cast<BlockedCallback *>(context);
  callback.calls++;
  callback.entered.set_value();
  callback.release.wait();
}

void CloseWaitsForCallbacksAndPreventsLaterEvents() {
  std::promise<void> release;
  BlockedCallback callback;
  callback.release = release.get_future().share();
  auto entered = callback.entered.get_future();
  const auto session = mrec_tracks_open(Block, &callback);
  assert(mrec_track_started(session, MREC_TRACK_MICROPHONE, 1, "Input"));
  std::thread publisher([session] {
    mrec_track_interrupted(session, MREC_TRACK_MICROPHONE, 0,
                           MREC_TRACK_REASON_DEVICE_REMOVED, "lost");
  });
  assert(entered.wait_for(5s) == std::future_status::ready);
  std::promise<void> closing;
  auto stopping = std::async(std::launch::async, [&closing] {
    closing.set_value();
    mrec_tracks_close();
  });
  closing.get_future().wait();
  assert(stopping.wait_for(20ms) == std::future_status::timeout);
  release.set_value();
  publisher.join();
  assert(stopping.wait_for(5s) == std::future_status::ready);
  stopping.get();
  mrec_track_resumed(session, MREC_TRACK_MICROPHONE, 0, "Replacement");
  mrec_track_interrupted(session, MREC_TRACK_MICROPHONE, 0,
                         MREC_TRACK_REASON_DEVICE_REMOVED, "late");
  assert(callback.calls == 1);
}

/* Well inside the retry interval, so only a wake or a stop explains it. */
constexpr auto kPrompt = std::chrono::milliseconds(MREC_TRACK_RETRY_INTERVAL_MS / 2);

struct Retry {
  std::atomic<int> attempts{0};
  std::atomic<bool> reopen{false};

  static int32_t Attempt(void *context, char *, size_t) {
    auto &retry = *static_cast<Retry *>(context);
    retry.attempts++;
    return retry.reopen ? 1 : 0;
  }

  /* Recovers `source` on another thread once the first attempt has failed. */
  std::future<int32_t> Start(uint64_t session, mrec_track track, uint32_t source) {
    auto result = std::async(std::launch::async, [this, session, track, source] {
      return mrec_track_recover(session, track, source, MREC_TRACK_REASON_DEVICE_REMOVED,
                                "lost", Attempt, this);
    });
    while (attempts == 0) std::this_thread::yield();
    std::this_thread::sleep_for(20ms);
    return result;
  }
};

void WakeFromAnotherThreadRetriesAtOnce() {
  Retry retry;
  const auto session = mrec_tracks_open(nullptr, nullptr);
  assert(mrec_track_started(session, MREC_TRACK_MICROPHONE, 1, "Input"));
  auto recovering = retry.Start(session, MREC_TRACK_MICROPHONE, 0);
  retry.reopen = true;
  mrec_tracks_wake(session);
  assert(recovering.wait_for(kPrompt) == std::future_status::ready);
  assert(recovering.get() == 1 && retry.attempts == 2);
  mrec_tracks_close();
}

void StopEndsAPendingRetryAtOnce() {
  Retry retry;
  const auto session = mrec_tracks_open(nullptr, nullptr);
  assert(mrec_track_started(session, MREC_TRACK_MICROPHONE, 1, "Input"));
  auto recovering = retry.Start(session, MREC_TRACK_MICROPHONE, 0);
  mrec_tracks_close();
  assert(recovering.wait_for(kPrompt) == std::future_status::ready);
  assert(recovering.get() == 0 && retry.attempts == 1);
}

void ExitEndsAPendingSystemRetryAtOnce() {
  Retry retry;
  const auto session = mrec_tracks_open(nullptr, nullptr);
  assert(mrec_track_started(session, MREC_TRACK_SYSTEM_AUDIO, 2, nullptr));
  auto recovering = retry.Start(session, MREC_TRACK_SYSTEM_AUDIO, 1);
  mrec_track_interrupted(session, MREC_TRACK_SYSTEM_AUDIO, MREC_TRACK_WHOLE,
                         MREC_TRACK_REASON_APP_EXITED, "exited");
  assert(recovering.wait_for(kPrompt) == std::future_status::ready);
  assert(recovering.get() == 0 && retry.attempts == 1);
  mrec_tracks_close();
}

void ReplacementEndsAPendingRetryAtOnce() {
  Retry retry;
  Events events;
  const auto session = mrec_tracks_open(nullptr, nullptr);
  assert(mrec_track_started(session, MREC_TRACK_MICROPHONE, 1, "Input"));
  auto recovering = retry.Start(session, MREC_TRACK_MICROPHONE, 0);
  retry.reopen = true;
  const auto replacement = mrec_tracks_open(Record, &events);
  assert(mrec_track_started(replacement, MREC_TRACK_MICROPHONE, 1, "Input"));
  assert(recovering.wait_for(kPrompt) == std::future_status::ready);
  assert(recovering.get() == 0 && retry.attempts == 1);
  mrec_tracks_close();
  assert(events.count == 0);
}
} // namespace

int main() {
  ConcurrentSourcesPublishSerialTransitions();
  CloseWaitsForCallbacksAndPreventsLaterEvents();
  WakeFromAnotherThreadRetriesAtOnce();
  StopEndsAPendingRetryAtOnce();
  ExitEndsAPendingSystemRetryAtOnce();
  ReplacementEndsAPendingRetryAtOnce();
  std::cout << "track concurrency tests passed\n";
}
