#include "../windows/AudioSessionNotifications.h"
#include "../windows/AudioThreadScheduling.h"

#include <cassert>
#include <thread>

int main() {
  auto signal = std::make_shared<meeting_record::RescanSignal>();
  const auto initial = signal->Revision();
  // A notification arriving before the wait must not be lost.
  signal->Notify();
  assert(signal->Wait(std::chrono::steady_clock::now() + std::chrono::seconds(1), initial) != initial);
  const auto revision = signal->Revision();
  std::thread producer([signal] { signal->Notify(); });
  assert(signal->Wait(std::chrono::steady_clock::now() + std::chrono::seconds(1), revision) != revision);
  producer.join();

  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  assert(SUCCEEDED(apartment));
  // Registration/teardown enumerate sessions without opening a capture stream.
  for (int i = 0; i < 5; ++i) {
    meeting_record::AudioSessionNotifications notifications(signal);
    notifications.Refresh();
    notifications.Refresh();
  }
  const int priority = GetThreadPriority(GetCurrentThread());
  { meeting_record::AudioThreadScheduling scheduling; }
  assert(GetThreadPriority(GetCurrentThread()) == priority);
  CoUninitialize();
}
