"""Missing-source startup and real audio-service restart through the C ABI."""
import ctypes as c
import os
from pathlib import Path
import subprocess
import tempfile
import time


class Event(c.Structure):
    _fields_ = [("track", c.c_int), ("kind", c.c_int), ("reason", c.c_int),
                ("timestamp", c.c_uint64), ("device", c.c_char * 256), ("message", c.c_char * 512)]


def until(predicate, description, seconds=10):
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, description
        time.sleep(0.03)


with tempfile.TemporaryDirectory(prefix="mrec-recovery-") as runtime:
    os.environ["XDG_RUNTIME_DIR"] = runtime
    os.environ["PULSE_RUNTIME_PATH"] = runtime + "/pulse"
    os.environ["PULSE_SERVER"] = "unix:" + runtime + "/pulse/native"
    Path(runtime, "pulse").mkdir()
    lib = c.CDLL(str(Path(os.environ["MREC_LINUX_BUILD"]) / "libmeetingrecord.so"))
    audio = c.CFUNCTYPE(None, c.POINTER(c.c_float), c.c_uint32, c.c_uint32, c.c_double, c.c_uint64, c.c_void_p)
    event_callback = c.CFUNCTYPE(None, c.POINTER(Event), c.c_void_p)
    lib.mrec_start_tracks_raw.argtypes = [c.c_void_p, c.c_size_t, c.c_int32, c.c_int32, c.c_int32, c.c_int32,
                                         c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p]
    frames = [0, 0]
    events = []

    def receive(index):
        def read(data, count, channels, rate, timestamp, context):
            frames[index] += count
        return audio(read)

    system, mic = receive(0), receive(1)

    @event_callback
    def on_event(pointer, context):
        event = pointer.contents
        events.append((event.track, event.kind, event.reason))

    def start(microphone):
        return lib.mrec_start_tracks_raw(None, 0, 1, 1, 0, microphone,
                                         system, None, mic if microphone else None, None, on_event, None)

    def server(microphone):
        command = ["pulseaudio", "-n", "--daemonize=no", "--use-pid-file=no", "--exit-idle-time=-1",
                   "--log-target=file:" + runtime + "/server.log",
                   "--load=module-native-protocol-unix socket=" + runtime + "/pulse/native auth-anonymous=1",
                   "--load=module-null-sink sink_name=mrec_output rate=48000"]
        if microphone:
            command += ["--load=module-remap-source master=mrec_output.monitor source_name=mrec_input"]
        child = subprocess.Popen(command)
        try:
            until(lambda: subprocess.run(["pactl", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0,
                  "private audio server startup")
        except BaseException:
            print(Path(runtime, "server.log").read_text(), flush=True)
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
            raise
        return child

    process = None
    try:
        # No server is different from a successful empty enumeration.
        assert start(0) == -7
        assert lib.mrec_is_running() == 0
        process = server(False)
        assert start(1) == -7, "missing microphone must fail startup"
        assert lib.mrec_is_running() == 0
        for _ in range(3):
            assert start(0) == 0, "failed startup retained the singleton"
            until(lambda: frames[0] > 0, "idle system silence")
            assert lib.mrec_stop() == 0
        process.terminate()
        process.wait(timeout=5)
        process = server(True)
        assert start(1) == 0
        until(lambda: frames[1] > 4800, "initial microphone capture")
        events.clear()
        process.terminate()
        process.wait(timeout=5)
        until(lambda: (0, 0, 5) in events and (1, 0, 5) in events, "service reset events")
        time.sleep(0.1)
        interrupted_frames = list(frames)
        time.sleep(0.2)
        assert frames == interrupted_frames, "audio continued while the service was unavailable"
        process = server(True)
        until(lambda: (0, 1, 0) in events and (1, 1, 0) in events, "both tracks resume after restart")
        until(lambda: frames[0] > interrupted_frames[0] + 4800 and frames[1] > interrupted_frames[1] + 4800,
              "audio resumes after service restart")
        assert lib.mrec_stop() == 0
        stopped = (list(frames), list(events))
        time.sleep(0.2)
        assert stopped == (frames, events), "callback arrived after stop"
        print("missing server/microphone, failed-start cleanup, idle audio, service restart, and stop ordering passed", flush=True)
    finally:
        if lib.mrec_is_running():
            lib.mrec_stop()
        if process and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
