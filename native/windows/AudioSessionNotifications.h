#pragma once

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace meeting_record {

// Callbacks only wake the watcher. COM enumeration and UI reads stay on its worker.
class RescanSignal {
public:
  void Notify() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++revision_;
    changed_.notify_all();
  }
  uint64_t Revision() {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
  }
  uint64_t Wait(std::chrono::steady_clock::time_point deadline, uint64_t revision) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_until(lock, deadline, [&] { return revision_ != revision; });
    return revision_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t revision_ = 0;
};

class AudioSessionNotifications {
  using Signal = std::shared_ptr<RescanSignal>;
  class SessionEvents : public Microsoft::WRL::RuntimeClass<
      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
      Microsoft::WRL::FtmBase, IAudioSessionEvents> {
  public:
    explicit SessionEvents(Signal signal) : signal_(std::move(signal)) {}
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float *, DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState) override { signal_->Notify(); return S_OK; }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
      signal_->Notify(); return S_OK;
    }
  private:
    Signal signal_;
  };
  class CreatedEvents : public Microsoft::WRL::RuntimeClass<
      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
      Microsoft::WRL::FtmBase, IAudioSessionNotification> {
  public:
    explicit CreatedEvents(Signal signal) : signal_(std::move(signal)) {}
    STDMETHODIMP OnSessionCreated(IAudioSessionControl *) override { signal_->Notify(); return S_OK; }
  private:
    Signal signal_;
  };
  class DeviceEvents : public Microsoft::WRL::RuntimeClass<
      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
      Microsoft::WRL::FtmBase, IMMNotificationClient> {
  public:
    explicit DeviceEvents(Signal signal) : signal_(std::move(signal)) {}
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return Changed(); }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return Changed(); }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return Changed(); }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return Changed(); }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return Changed(); }
  private:
    HRESULT Changed() { signal_->Notify(); return S_OK; }
    Signal signal_;
  };
  struct Session {
    Microsoft::WRL::ComPtr<IAudioSessionControl> control;
    Microsoft::WRL::ComPtr<SessionEvents> events;
    ~Session() { if (events) control->UnregisterAudioSessionNotification(events.Get()); }
  };
  struct Endpoint {
    Microsoft::WRL::ComPtr<IAudioSessionManager2> manager;
    Microsoft::WRL::ComPtr<CreatedEvents> events;
    std::vector<std::unique_ptr<Session>> sessions;
    ~Endpoint() { if (events) manager->UnregisterSessionNotification(events.Get()); }
  };

public:
  explicit AudioSessionNotifications(Signal signal) : signal_(std::move(signal)) {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator_)))) return;
    auto events = Microsoft::WRL::Make<DeviceEvents>(signal_);
    if (events && SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(events.Get())))
      devices_ = std::move(events);
  }
  ~AudioSessionNotifications() {
    endpoints_.clear();
    if (devices_) enumerator_->UnregisterEndpointNotificationCallback(devices_.Get());
  }

  void Refresh() {
    endpoints_.clear();
    if (!enumerator_) return;
    Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator_->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &devices))) return;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
      Microsoft::WRL::ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(index, &device))) continue;
      auto endpoint = std::make_unique<Endpoint>();
      if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                  reinterpret_cast<void **>(endpoint->manager.GetAddressOf())))) continue;
      auto created = Microsoft::WRL::Make<CreatedEvents>(signal_);
      if (created && SUCCEEDED(endpoint->manager->RegisterSessionNotification(created.Get())))
        endpoint->events = std::move(created);
      Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
      if (FAILED(endpoint->manager->GetSessionEnumerator(&sessions))) continue;
      int size = 0;
      // GetCount also arms session-creation notifications, including an empty list.
      sessions->GetCount(&size);
      for (int i = 0; i < size; ++i) {
        auto session = std::make_unique<Session>();
        if (FAILED(sessions->GetSession(i, &session->control))) continue;
        auto events = Microsoft::WRL::Make<SessionEvents>(signal_);
        if (events && SUCCEEDED(session->control->RegisterAudioSessionNotification(events.Get()))) {
          session->events = std::move(events);
          endpoint->sessions.push_back(std::move(session));
        }
      }
      endpoints_.push_back(std::move(endpoint));
    }
  }

private:
  Signal signal_;
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<DeviceEvents> devices_;
  std::vector<std::unique_ptr<Endpoint>> endpoints_;
};

} // namespace meeting_record
