#pragma once
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace meeting_record {
struct RenderEndpoints {
  std::vector<std::wstring> active;
  std::vector<std::wstring> wanted;
};

inline bool HasExternalRenderSession(IMMDevice *device) {
  Microsoft::WRL::ComPtr<IAudioSessionManager2> manager;
  Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
  if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
      reinterpret_cast<void **>(manager.GetAddressOf()))) ||
      FAILED(manager->GetSessionEnumerator(&sessions))) return false;
  int count = 0;
  if (FAILED(sessions->GetCount(&count))) return false;
  for (int i = 0; i < count; i++) {
    Microsoft::WRL::ComPtr<IAudioSessionControl> control;
    Microsoft::WRL::ComPtr<IAudioSessionControl2> session;
    AudioSessionState state = AudioSessionStateInactive;
    DWORD pid = 0;
    if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control.As(&session)) &&
        SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive &&
        SUCCEEDED(session->GetProcessId(&pid)) && pid != GetCurrentProcessId()) return true;
  }
  return false;
}

inline std::optional<RenderEndpoints> RenderEndpointIds() {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
      IID_PPV_ARGS(&enumerator))) ||
      FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return std::nullopt;
  UINT count = 0;
  if (FAILED(devices->GetCount(&count))) return std::nullopt;
  std::vector<std::wstring> defaults;
  for (ERole role : {eConsole, eMultimedia, eCommunications}) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    LPWSTR id = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device)) &&
        SUCCEEDED(device->GetId(&id))) {
      defaults.emplace_back(id);
      CoTaskMemFree(id);
    }
  }
  RenderEndpoints result;
  for (UINT i = 0; i < count; i++) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    LPWSTR id = nullptr;
    if (FAILED(devices->Item(i, &device)) || FAILED(device->GetId(&id))) return std::nullopt;
    const std::wstring endpoint_id(id);
    result.active.push_back(endpoint_id);
    // Opening idle alternative endpoints can reserve shared driver resources
    // needed by a restored output. Our own loopback session must not keep an
    // otherwise idle device open indefinitely.
    if (std::find(defaults.begin(), defaults.end(), endpoint_id) != defaults.end() ||
        HasExternalRenderSession(device.Get())) result.wanted.push_back(endpoint_id);
    CoTaskMemFree(id);
  }
  return result;
}

inline HRESULT OpenEndpointLoopback(const std::wstring &id, uint32_t channels,
    IAudioClient **client, IAudioCaptureClient **capture) {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  Microsoft::WRL::ComPtr<IMMDevice> device;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) return hr;
  hr = enumerator->GetDevice(id.c_str(), &device);
  if (FAILED(hr)) return hr;
  Microsoft::WRL::ComPtr<IAudioClient> opened;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void **>(opened.GetAddressOf()));
  if (FAILED(hr)) return hr;
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = static_cast<WORD>(channels);
  format.nSamplesPerSec = 48000;
  format.wBitsPerSample = 32;
  format.nBlockAlign = format.nChannels * 4;
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
  hr = opened->Initialize(AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 10000000, 0, &format, nullptr);
  if (FAILED(hr)) return hr;
  hr = opened->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(capture));
  if (SUCCEEDED(hr)) *client = opened.Detach();
  return hr;
}
} // namespace meeting_record
