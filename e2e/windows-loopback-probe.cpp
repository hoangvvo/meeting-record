// Raw virtual-loopback diagnostics, independent of the library/mixer/bindings.
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;
class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HRESULT result = E_PENDING;
  ComPtr<IAudioClient> client;
  ~Activation() { if (event) CloseHandle(event); }
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override {
    ComPtr<IUnknown> object;
    HRESULT activation = E_FAIL;
    result = operation->GetActivateResult(&activation, &object);
    if (SUCCEEDED(result)) result = activation;
    if (SUCCEEDED(result)) result = object.As(&client);
    SetEvent(event);
    return S_OK;
  }
};

static bool Check(HRESULT hr, const char *what) {
  if (SUCCEEDED(hr)) return true;
  fprintf(stderr, "%s: 0x%08lx\n", what, static_cast<unsigned long>(hr));
  return false;
}

static int Probe(DWORD pid, bool exclude, DWORD seconds, const wchar_t *endpoint_role) {
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = exclude
      ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
      : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT blob{};
  blob.vt = VT_BLOB;
  blob.blob.cbSize = sizeof params;
  blob.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
  ComPtr<IAudioClient> client;
  if (endpoint_role) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (!Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          IID_PPV_ARGS(&enumerator)), "enumerator") ||
        !Check(enumerator->GetDefaultAudioEndpoint(eRender,
          std::wstring(endpoint_role) == L"communications" ? eCommunications : eConsole,
          &device), "endpoint") ||
        !Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, &blob,
          reinterpret_cast<void **>(client.GetAddressOf())), "filtered endpoint activation")) return 1;
  } else {
    auto handler = Microsoft::WRL::Make<Activation>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    if (!handler->event || !Check(ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &blob,
        handler.Get(), &operation), "activation")) return 1;
    if (WaitForSingleObject(handler->event, 5000) != WAIT_OBJECT_0 ||
        !Check(handler->result, "activation result")) return 1;
    client = handler->client;
  }
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = 2;
  format.nSamplesPerSec = 48000;
  format.wBitsPerSample = 32;
  format.nBlockAlign = 8;
  format.nAvgBytesPerSec = 384000;
  if (!Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &format, nullptr), "initialize")) return 1;
  ComPtr<IAudioCaptureClient> capture;
  if (!Check(client->GetService(IID_PPV_ARGS(&capture)), "capture service") ||
      !Check(client->Start(), "start")) return 1;
  uint64_t frames = 0, packets = 0, silent = 0, timestamp_errors = 0;
  double peak = 0;
  double sine[2]{}, cosine[2]{};
  const double frequencies[2]{997, 440};
  const ULONGLONG end = GetTickCount64() + seconds * 1000ull;
  while (GetTickCount64() < end) {
    UINT32 available = 0;
    if (!Check(capture->GetNextPacketSize(&available), "next packet")) return 1;
    if (!available) { Sleep(3); continue; }
    BYTE *data = nullptr;
    UINT32 count = 0;
    DWORD flags = 0;
    UINT64 position = 0, time = 0;
    if (!Check(capture->GetBuffer(&data, &count, &flags, &position, &time), "buffer")) return 1;
    packets++;
    if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) timestamp_errors++;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) silent++;
    else if (data) {
      const float *samples = reinterpret_cast<float *>(data);
      for (UINT32 i = 0; i < count; i++) {
        const double sample = samples[i * 2];
        peak = (std::max)(peak, std::abs(sample));
        for (int tone = 0; tone < 2; tone++) {
          const double phase = 6.283185307179586 * frequencies[tone] * (frames + i) / 48000.0;
          sine[tone] += sample * sin(phase);
          cosine[tone] += sample * cos(phase);
        }
      }
    }
    frames += count;
    if (!Check(capture->ReleaseBuffer(count), "release")) return 1;
  }
  if (!Check(client->Stop(), "stop")) return 1;
  printf("raw path=%s mode=%s pid=%lu packets=%llu frames=%llu silent=%llu timestamp_errors=%llu peak=%.6f",
         endpoint_role ? "filtered-endpoint" : "virtual", exclude ? "exclude" : "include", static_cast<unsigned long>(pid),
         static_cast<unsigned long long>(packets), static_cast<unsigned long long>(frames),
         static_cast<unsigned long long>(silent), static_cast<unsigned long long>(timestamp_errors), peak);
  for (int tone = 0; tone < 2; tone++) {
    printf(" tone%.0f_amplitude=%.6f", frequencies[tone], frames ?
        2 * sqrt(sine[tone] * sine[tone] + cosine[tone] * cosine[tone]) / frames : 0);
  }
  printf("\n");
  // Zero signal is a diagnostic observation, not a failed API call.
  return frames ? 0 : 1;
}

int wmain(int argc, wchar_t **argv) {
  if ((argc != 4 && argc != 5) || (std::wstring(argv[1]) != L"include" && std::wstring(argv[1]) != L"exclude")) {
    fprintf(stderr, "usage: windows-loopback-probe include|exclude pid|self seconds [console|communications]\n");
    return 2;
  }
  DWORD pid = std::wstring(argv[2]) == L"self" ? GetCurrentProcessId() : wcstoul(argv[2], nullptr, 10);
  DWORD seconds = wcstoul(argv[3], nullptr, 10);
  if (!pid || seconds < 1 || seconds > 60) return 2;
  if (argc == 5 && std::wstring(argv[4]) != L"console" && std::wstring(argv[4]) != L"communications") return 2;
  if (!Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM")) return 1;
  const int result = Probe(pid, std::wstring(argv[1]) == L"exclude", seconds, argc == 5 ? argv[4] : nullptr);
  CoUninitialize();
  return result;
}
