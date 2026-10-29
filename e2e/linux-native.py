"""Direct ABI capture probe, independent of Node/Rust delivery queues."""
import ctypes as c
import os
from pathlib import Path
import time
import threading

lib = c.CDLL(str(Path(os.environ["MREC_LINUX_BUILD"]) / "libmeetingrecord.so"))
audio = c.CFUNCTYPE(None, c.POINTER(c.c_float), c.c_uint32, c.c_uint32, c.c_double, c.c_uint64, c.c_void_p)
frames = [0, 0]
last = [0, 0]
malformed = []


def callback(index):
    def read(data, count, channels, rate, timestamp, context):
        if channels != 1 or rate != 48000 or timestamp < last[index] or timestamp > time.monotonic_ns() + 20_000_000:
            malformed.append((index, channels, rate, timestamp))
        last[index] = timestamp
        frames[index] += count
    return audio(read)


system, mic = callback(0), callback(1)
pid = c.c_uint32(int(os.environ["TEST_PID_A"]))
lib.mrec_start_tracks_raw.argtypes = [c.POINTER(c.c_uint32), c.c_size_t, c.c_int32, c.c_int32, c.c_int32, c.c_int32, audio, c.c_void_p, audio, c.c_void_p, c.c_void_p, c.c_void_p]
assert lib.mrec_start_tracks_raw(c.byref(pid), 1, 0, 1, 0, 1, system, None, mic, None, None, None) == 0
time.sleep(3)
assert lib.mrec_stop() == 0
assert not malformed, malformed
assert min(frames) > 96000, frames
print("native ABI frames", frames, flush=True)

# Callback pacing/backpressure must not trap stop inside a continuously refilled
# Pulse read notification. Already accepted mixer blocks still drain in order.
slow_frames = [0]


@audio
def slow(data, count, channels, rate, timestamp, context):
    slow_frames[0] += count
    time.sleep(0.03)


assert lib.mrec_start_tracks_raw(c.byref(pid), 1, 0, 1, 0, 0, slow, None, audio(), None, None, None) == 0
time.sleep(0.5)
stopped = threading.Event()
stop_status = []


def stop_slow_capture():
    stop_status.append(lib.mrec_stop())
    stopped.set()


threading.Thread(target=stop_slow_capture, daemon=True).start()
if not stopped.wait(3):
    print("stop timed out with a slow audio consumer", flush=True)
    os._exit(1)
assert stop_status == [0], stop_status
assert slow_frames[0] > 0
final_frames = slow_frames[0]
time.sleep(0.1)
assert slow_frames[0] == final_frames, "audio arrived after stop"
print("native stop remains bounded with a slow consumer", flush=True)
