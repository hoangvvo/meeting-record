// Deterministic audio peer for Linux E2E. Run independently of the recorder.
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <thread>
#include <unistd.h>
static std::atomic<bool> running{true};
static void Stop(int) { running = false; }
int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  signal(SIGTERM, Stop);
  signal(SIGINT, Stop);
  const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, 48000, 2};
  // Bound test-player latency and avoid multi-second rebuffering after
  // underruns.
  const pa_buffer_attr buffering{48000 * 8, 4800 * 8, 0, 480 * 8, UINT32_MAX};
  int error = 0;
  auto stream =
      pa_simple_new(nullptr, "Linux E2E tone", PA_STREAM_PLAYBACK, argv[1],
                    "deterministic tone", &format, nullptr, &buffering, &error);
  if (!stream) {
    std::fprintf(stderr, "%s\n", pa_strerror(error));
    return 1;
  }
  std::thread mic;
  if (argc > 3)
    mic = std::thread([&] {
      int input_error = 0;
      auto input = pa_simple_new(nullptr, "Linux E2E tone", PA_STREAM_RECORD,
                                 argv[3], "call microphone", &format, nullptr,
                                 nullptr, &input_error);
      if (!input)
        std::abort();
      float buffer[960];
      while (running &&
             pa_simple_read(input, buffer, sizeof buffer, nullptr) >= 0) {
      }
      pa_simple_free(input);
    });
  const double frequency = std::strtod(argv[2], nullptr);
  uint64_t frame = 0;
  while (running) {
    float buffer[960];
    for (int i = 0; i < 480; ++i, ++frame) {
      buffer[i * 2] = float(0.15 * std::sin(2 * 3.141592653589793 * frequency *
                                            double(frame) / 48000));
      buffer[i * 2 + 1] = buffer[i * 2];
    }
    if (pa_simple_write(stream, buffer, sizeof buffer, &error) < 0)
      break;
  }
  running = false;
  pa_simple_free(stream);
  if (mic.joinable())
    mic.join();
}
