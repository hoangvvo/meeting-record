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
} // namespace

int main() {
  ConcurrentSourcesPublishSerialTransitions();
  CloseWaitsForCallbacksAndPreventsLaterEvents();
  std::cout << "track concurrency tests passed\n";
}
