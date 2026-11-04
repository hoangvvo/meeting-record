// Explicit WASAPI render categories for capture coverage tests. No capture code.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char *operation) {
  if (FAILED(hr)) {
    char message[160];
    snprintf(message, sizeof message, "%s failed: 0x%08lx", operation,
             static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
  }
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 6 && argc != 7) {
    fprintf(stderr, "usage: windows-render-tone console|communications ordinary|communications frequency seconds ready-file [reference]\n");
    return 2;
  }
  const bool communications_role = std::wstring(argv[1]) == L"communications";
  const bool communications_category = std::wstring(argv[2]) == L"communications";
  const int frequency = _wtoi(argv[3]);
  const int seconds = _wtoi(argv[4]);
  if ((std::wstring(argv[1]) != L"console" && !communications_role) ||
      (std::wstring(argv[2]) != L"ordinary" && !communications_category) ||
      frequency < 100 || frequency > 5000 || seconds < 1 || seconds > 600) return 2;
  HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) return 1;
  int result = 0;
  try {
    ComPtr<IMMDeviceEnumerator> enumerator;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)), "enumerator");
    ComPtr<IMMDevice> device;
    Check(enumerator->GetDefaultAudioEndpoint(eRender,
          communications_role ? eCommunications : eConsole, &device), "default output");
    LPWSTR id = nullptr;
    Check(device->GetId(&id), "device id");
    wprintf(L"endpoint=%ls role=%ls category=%ls frequency=%d\n", id, argv[1], argv[2], frequency);
    CoTaskMemFree(id);

    ComPtr<IAudioClient2> client;
    Check(device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr,
          reinterpret_cast<void **>(client.GetAddressOf())), "activate render");
    AudioClientProperties properties{};
    properties.cbSize = sizeof properties;
    properties.eCategory = communications_category ? AudioCategory_Communications : AudioCategory_Other;
    Check(client->SetClientProperties(&properties), "set render category");
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = 384000;
    Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          1000000, 0, &format, nullptr), "initialize render");
    // Independent endpoint reference distinguishes a missing render source from
    // missing process-loopback coverage. Optional: it must not mask baseline behavior.
    ComPtr<IAudioClient> reference;
    ComPtr<IAudioCaptureClient> reference_packets;
    if (argc == 7) {
      Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void **>(reference.GetAddressOf())), "activate reference");
      Check(reference->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            1000000, 0, &format, nullptr), "initialize reference");
      Check(reference->GetService(IID_PPV_ARGS(&reference_packets)), "reference service");
      Check(reference->Start(), "start reference");
    }
    ComPtr<IAudioRenderClient> render;
    Check(client->GetService(IID_PPV_ARGS(&render)), "render service");
    UINT32 capacity = 0;
    Check(client->GetBufferSize(&capacity), "render capacity");
    uint64_t frame = 0;
    auto fill = [&] {
      UINT32 padding = 0;
      Check(client->GetCurrentPadding(&padding), "render padding");
      const UINT32 count = capacity - padding;
      if (!count) return;
      BYTE *bytes = nullptr;
      Check(render->GetBuffer(count, &bytes), "render buffer");
      float *samples = reinterpret_cast<float *>(bytes);
      for (UINT32 i = 0; i < count; ++i, ++frame) {
        const float sample = static_cast<float>(0.15 * sin(6.283185307179586 * frequency * frame / 48000.0));
        samples[i * 2] = samples[i * 2 + 1] = sample;
      }
      Check(render->ReleaseBuffer(count, 0), "release render");
    };
    fill();
    Check(client->Start(), "start render");
    HANDLE ready = CreateFileW(argv[5], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ready == INVALID_HANDLE_VALUE) throw std::runtime_error("create ready file failed");
    CloseHandle(ready);
    fflush(stdout);
    const ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    ULONGLONG next_report = GetTickCount64() + 1000;
    uint64_t reference_frames = 0;
    double reference_peak = 0;
    double sine_sum = 0, cosine_sum = 0;
    while (GetTickCount64() < end) {
      fill();
      if (reference_packets) {
        UINT32 available = 0;
        Check(reference_packets->GetNextPacketSize(&available), "reference size");
        while (available) {
          BYTE *data = nullptr;
          UINT32 count = 0;
          DWORD flags = 0;
          Check(reference_packets->GetBuffer(&data, &count, &flags, nullptr, nullptr), "reference buffer");
          const float *samples = reinterpret_cast<float *>(data);
          for (UINT32 i = 0; i < count; ++i, ++reference_frames) {
            const double sample = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : samples[i * 2];
            reference_peak = (std::max)(reference_peak, std::abs(sample));
            const double phase = 6.283185307179586 * frequency * reference_frames / 48000.0;
            sine_sum += sample * sin(phase);
            cosine_sum += sample * cos(phase);
          }
          Check(reference_packets->ReleaseBuffer(count), "release reference");
          Check(reference_packets->GetNextPacketSize(&available), "reference next size");
        }
        if (GetTickCount64() >= next_report) {
          const double amplitude = reference_frames ? 2 * sqrt(sine_sum * sine_sum + cosine_sum * cosine_sum) / reference_frames : 0;
          printf("reference frames=%llu peak=%.6f tone%d_amplitude=%.6f\n",
                 static_cast<unsigned long long>(reference_frames), reference_peak, frequency, amplitude);
          fflush(stdout);
          next_report += 1000;
        }
      }
      Sleep(3);
    }
    Check(client->Stop(), "stop render");
    if (reference) Check(reference->Stop(), "stop reference");
  } catch (const std::exception &error) {
    fprintf(stderr, "%s\n", error.what());
    result = 1;
  }
  DeleteFileW(argv[5]);
  CoUninitialize();
  return result;
}
