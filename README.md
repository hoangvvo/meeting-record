# meeting-record

System-audio capture and meeting detection for macOS and Windows.

Records the audio of a call without a virtual audio device, a kernel extension, or
screen recording. Requires one permission, granted once.

```c
/* detect a call, then record the processes it is using */
static void on_meeting(const mrec_meeting *m, mrec_event event, void *ud) {
  if (event == MREC_MEETING_STARTED && m->should_record) {
    mrec_start_meeting(m, on_audio, NULL);
  } else if (event == MREC_MEETING_ENDED) {
    mrec_stop();
  }
}

mrec_watch_start(on_meeting, NULL);
```

Three bindings over one core:

| language | entry point | install |
|---|---|---|
| **TypeScript / Node** | `meeting-record` | `npm install meeting-record` |
| **Rust** | `meeting_record` | `cargo add meeting-record` |
| **C** | `native/include/meeting-record.h` | link the static library |

The Node and Rust layers are thin wrappers over the same C ABI, so behaviour is
identical across all three. Design rationale is in
[docs/NODE-API.md](docs/NODE-API.md); platform mechanics and failure modes in
[docs/INTERNALS.md](docs/INTERNALS.md).

---

## Contents

- [Layout](#layout)
- [Install](#install)
- [Quickstart](#quickstart)
- [Permissions](#permissions)
- [Detecting meetings](#detecting-meetings)
- [Capturing audio](#capturing-audio)
- [API reference](#api-reference)
- [Rules you must follow](#rules-you-must-follow)
- [Platform support](#platform-support)
- [Troubleshooting](#troubleshooting)
- [Status](#status)

---

## Layout

A monorepo: one native core, one binding per language.

```
native/          C ABI core — Swift (macOS) + C++ (Windows)
  include/         meeting-record.h, meeting-record-detect.h
  macos/ windows/  platform implementations
  examples/ tests/ C and Swift checks for the core
node/            npm package  "meeting-record"   (N-API addon + TypeScript)
rust/            cargo crate  "meeting-record"   (cargo workspace member)
scripts/         build-macos.sh
docs/
```

Both bindings link the same static library, so there is no duplicated logic and no
behavioural drift between them.

## Install

Requires macOS 14.2+ (Apple silicon or Intel) or Windows 10 build 20348+.

```bash
npm install meeting-record     # Node
cargo add meeting-record       # Rust
```

From a clone:

```bash
git clone <this repo> && cd meeting-record
npm run build                  # native core + addon + TypeScript + Rust
```

For C, `./scripts/build-macos.sh` produces
`native/build/libmeetingrecord_macos.a`. Link it with:

```bash
clang myapp.c native/build/libmeetingrecord_macos.a \
  -I native/include \
  -framework CoreAudio -framework AVFoundation -framework AudioToolbox \
  -framework Foundation -framework AppKit -framework ApplicationServices \
  -L"$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx" \
  -lswiftCore
```

On Windows, compile `native/windows/*.cpp` and link `ole32 mmdevapi user32`.

### Your app bundle needs this

Capture will not work without it — the system refuses to even prompt.

```xml
<key>NSAudioCaptureUsageDescription</key>
<string>Records meeting audio so it can be transcribed.</string>
```

---

## Quickstart

### TypeScript

```ts
import { createWriteStream } from 'node:fs'
import mrec from 'meeting-record'

await mrec.permissions.request('system-audio')

mrec.autoRecord({
  onStart(session, meeting) {
    console.log(`recording ${meeting.platform} at ${session.sampleRate}Hz`)
    session.pipe(createWriteStream(`${meeting.platform}.f32`))
  },
  onStop(meeting) {
    console.log(`finished ${meeting.platform}`)
  },
})
```

Audio arrives as a `Readable` stream of interleaved float32, so it pipes anywhere
and gives you backpressure. Lower-level pieces are available too:

```ts
mrec.meetings.on('started', (m) => { /* m.shouldRecord, m.platform, m.title */ })
mrec.meetings.watch()

const session = await mrec.capture.start({ pids: [1234], mono: true })
session.on('drop', (n) => console.warn(`lost ${n} samples`))
await session.stop()
```

### Rust

Capture and watching return guards that stop on drop, so a running capture cannot
be leaked by an early return.

```rust
use meeting_record as mrec;

let _watcher = mrec::watch(|event, meeting| {
    if event == mrec::MeetingEvent::Started && meeting.should_record {
        // The handler below runs on a realtime thread: copy and return.
        let _capture = mrec::record(meeting, |buffer| {
            tx.send(buffer.frames.to_vec()).ok();
        });
    }
})?;
```

### C

The complete flow: ask for permission, wait for a meeting, record it to a file.

```c
#include "meeting-record.h"
#include "meeting-record-detect.h"

#include <stdio.h>
#include <unistd.h>

static FILE *g_out;

/* Realtime audio thread — write and return, nothing else. */
static void on_audio(const float *frames, uint32_t frame_count, uint32_t channels,
                     double sample_rate, uint64_t host_time_ns, void *ud) {
  fwrite(frames, sizeof(float), (size_t)frame_count * channels, g_out);
}

static void on_meeting(const mrec_meeting *m, mrec_event event,
                       void *ud) {
  if (event == MREC_MEETING_STARTED && m->confidence >= 70) {
    printf("%s meeting started — recording\n",
           mrec_platform_name(m->platform));

    g_out = fopen("meeting.f32", "wb");

    mrec_config cfg;
    mrec_config_defaults(&cfg);
    cfg.pids      = m->audio_pids;
    cfg.pid_count = m->audio_pid_count;

    if (mrec_start(&cfg, on_audio, NULL) != MREC_OK) {
      printf("start failed: %s\n", mrec_last_error());
    }
  } else if (event == MREC_MEETING_ENDED && mrec_is_running()) {
    printf("meeting ended — stopping\n");
    mrec_stop();
    fclose(g_out);
  }
}

int main(void) {
  if (mrec_audio_permission_status() != MREC_PERM_GRANTED) {
    mrec_request_audio_permission();          /* shows the one-time prompt */
    while (mrec_audio_permission_status() != MREC_PERM_GRANTED) sleep(1);
  }

  mrec_watch_start(on_meeting, NULL);
  pause();                                       /* your app's run loop */
}
```

The output is raw interleaved float32. To make it playable:

```bash
ffmpeg -f f32le -ar 48000 -ac 1 -i meeting.f32 meeting.wav
```

---

## Permissions

Two, and only the first is required.

| permission | needed for | prompt |
|---|---|---|
| **system audio** | capturing anything | one-time dialog |
| **accessibility** | meeting titles and URLs only | a trip to System Settings |

```c
mrec_permission status = mrec_audio_permission_status();
/* MREC_PERM_GRANTED | DENIED | UNKNOWN | NOT_REQUIRED (Windows) */

if (status != MREC_PERM_GRANTED) mrec_request_audio_permission();
```

`mrec_request_audio_permission()` returns immediately — the dialog is modal to
the user, not to you. Poll `mrec_audio_permission_status()` for the answer.

Three requirements for the prompt to appear at all:

1. `NSAudioCaptureUsageDescription` in your `Info.plist`
2. a code-signed bundle
3. a **GUI process** — your app must have a running `NSApplication`

Electron and native Mac apps satisfy (3) automatically. A bare CLI does not, and
in that case the underlying OS call *blocks* rather than failing. See
[Troubleshooting](#troubleshooting).

Detection works fine with no permission at all — you just get empty `title` and
`url` fields.

---

## Detecting meetings

### Event-driven (recommended)

```c
mrec_watch_start(on_meeting, user_data);
/* ... */
mrec_watch_stop();
```

Your callback receives `STARTED`, `UPDATED` and `ENDED`. It runs on an internal
serial queue, **not** a realtime thread, so allocating and calling back into a
managed runtime is fine here.

`UPDATED` fires when something actionable changes — title, URL, mic state, or the
audio pid set — not on every confidence wobble.

This is driven by app launch/terminate notifications plus a CoreAudio listener on
the process list, with a slow poll as a backstop. It is not a busy loop.

### Polling

```c
mrec_meeting meetings[8];
size_t count = 0;
mrec_scan(meetings, 8, &count);
```

Cheap enough to call every second or two.

### Reading the result

```c
typedef struct {
  mrec_platform platform;   /* ZOOM, TEAMS, MEET, WEBEX, SLACK, ... */
  uint32_t pid;                       /* the app the user sees */
  uint32_t audio_pids[16];            /* what you actually record */
  size_t   audio_pid_count;
  char     app_name[128];
  char     title[512];                /* empty without accessibility */
  char     url[1024];                 /* empty without accessibility */
  int32_t  is_using_mic;              /* the user can be heard */
  int32_t  is_playing_audio;          /* someone else can be heard */
  int32_t  confidence;                /* 0-100 */
  uint64_t detected_at_ns;
} mrec_meeting;
```

`pid` is the application the user sees; `audio_pids` is where the audio actually
comes from. Chrome, Electron and Teams render audio from helper processes, so the
two differ. `mrec_start_meeting()` handles this for you.

### Confidence

`known app 40 + audio output 30 + microphone 25 + recognised URL 5`

Use `should_record` rather than comparing `confidence` yourself; the weights may
change.

| score | situation | `should_record` |
|---|---|---|
| 40 | conferencing app open, idle | 0 |
| 70 | remote audio only (you are muted) | 1 |
| 95 | two-way audio | 1 |

A browser requires more evidence than a native client, because a video tab and a
call tab are indistinguishable at the process level: it must either match a known
meeting URL or have an active microphone.

---

## Capturing audio

```c
mrec_config cfg;
mrec_config_defaults(&cfg);      /* always start here */

cfg.pids      = pids;              /* from a meeting, or mrec_list_audio_processes */
cfg.pid_count = n;
cfg.mono      = 1;                 /* default; halves the data for speech */

mrec_status st = mrec_start(&cfg, on_audio, user_data);
if (st != MREC_OK) fprintf(stderr, "%s\n", mrec_last_error());
```

| field | default | notes |
|---|---|---|
| `pids` / `pid_count` | none | **preferred.** Keeps working when the user mutes their speakers |
| `global_mixdown` | `0` | whole system instead. Simpler, but records **silence while muted** |
| `mono` | `1` | mono mixdown |
| `mute_captured_output` | `0` | `1` also silences the app's own output — rarely what you want |
| `target_sample_rate` | `0` | `0` = native, 48000 in practice |

Leaving `pids` empty and `global_mixdown` at `0` captures every process currently
playing audio.

### The callback is realtime

```c
static void on_audio(const float *frames, uint32_t frame_count, uint32_t channels,
                     double sample_rate, uint64_t host_time_ns, void *ud);
```

`frames` is interleaved float32, `frame_count` frames per channel. It is only
valid for the duration of the call — copy what you need.

**Do not allocate, take a lock, do I/O, or call into a GC'd runtime here.** Push
into a lock-free ring buffer and process on your own thread. The `fwrite` in the
quickstart is a demo shortcut, not a pattern to copy.

### Finding processes yourself

```c
mrec_process procs[128];
size_t count = 0;
mrec_list_audio_processes(procs, 128, &count);

for (size_t i = 0; i < count; i++) {
  if (procs[i].is_running_output) {
    printf("%u %s (%s)\n", procs[i].pid, procs[i].name, procs[i].bundle_id);
  }
}
```

Only live processes are returned.

---

## API reference

### Audio — `meeting-record.h`

| function | purpose |
|---|---|
| `mrec_audio_permission_status()` | `GRANTED` / `DENIED` / `UNKNOWN` / `NOT_REQUIRED` |
| `mrec_request_audio_permission()` | show the one-time prompt; returns immediately |
| `mrec_list_audio_processes(out, cap, *count)` | live processes doing audio IO |
| `mrec_config_defaults(*cfg)` | fill a config with defaults |
| `mrec_start(*cfg, cb, ud)` | begin capture |
| `mrec_stop()` | end capture |
| `mrec_is_running()` | `1` while capturing |
| `mrec_current_format(*rate, *channels)` | negotiated format, once running |
| `mrec_last_error()` | detail for the last failure; never `NULL` |

### Detection — `meeting-record-detect.h`

| function | purpose |
|---|---|
| `mrec_scan(out, cap, *count)` | point-in-time scan, best confidence first |
| `mrec_watch_start(cb, ud)` | begin watching |
| `mrec_watch_stop()` | stop watching |
| `mrec_is_watching()` | `1` while watching |
| `mrec_accessibility_permission_status()` | for titles/URLs |
| `mrec_request_accessibility_permission()` | opens System Settings |
| `mrec_platform_name(p)` | display name; static, do not free |

### Status codes

`0` is success, everything negative is failure.

| code | meaning |
|---|---|
| `MREC_OK` | `0` |
| `MREC_ERR_UNSUPPORTED_OS` | `-1` macOS < 14.2 / Windows < 20348 |
| `MREC_ERR_PERMISSION` | `-2` not granted, or still undetermined |
| `MREC_ERR_ALREADY_RUNNING` | `-3` |
| `MREC_ERR_NOT_RUNNING` | `-4` |
| `MREC_ERR_NO_PROCESSES` | `-5` nothing to capture |
| `MREC_ERR_TAP_FAILED` | `-6` |
| `MREC_ERR_DEVICE_FAILED` | `-7` |
| `MREC_ERR_IOPROC_FAILED` | `-8` |
| `MREC_ERR_INTERNAL` | `-9` |
| `MREC_ERR_BUFFER_TOO_SMALL` | `-10` list truncated; `count` is still valid |

---

## Rules you must follow

1. **The audio callback is realtime.** No allocation, locks, I/O, or managed
   runtimes. The meeting callback is not realtime and has no such restriction.

2. **One capture and one watcher per process.** Both are process-wide singletons;
   a second `mrec_start()` returns `ALREADY_RUNNING`.

3. **Never `SIGKILL` a process that is capturing.** Always `mrec_stop()` first.
   Killing mid-capture leaks OS audio state and can eventually wedge `coreaudiod`
   for the whole machine. Install a signal handler.

4. **`mrec_start()` can block for up to 6 seconds** the first time, before the
   permission grant exists. Never call it on a UI thread.

5. **FFI callers must call `mrec_start_raw()`.** `mrec_start()` and
   `mrec_config_defaults()` are `static inline` in the header, not exported
   symbols, so `dlsym("mrec_start")` fails. Signature:
   `mrec_start_raw(pids, count, global, mono, mute, rate, cb, ud)`.

6. **`mrec_last_error()` is a single process-wide slot** shared by both modules
   and written from a worker thread on timeout. Read it immediately after a
   failure; do not treat it as thread-safe.

---

## Platform support

| | macOS | Windows |
|---|---|---|
| minimum | 14.2 | 10 build 20348 |
| mechanism | CoreAudio process taps | WASAPI process loopback |
| permission | one-time prompt | none |
| recording indicator | none | none |
| per-process capture | yes | yes |
| works while muted | yes | yes |
| meeting titles | needs accessibility | free |
| browser URLs | needs accessibility | not yet |

---

## Troubleshooting

**`mrec_start` returns `-2` with a timeout message.** The permission grant is
undetermined and the OS call blocked. Call
`mrec_request_audio_permission()` from a GUI process first, or grant it under
System Settings → Privacy & Security → Screen & System Audio Recording.

**Capture runs but every sample is zero.** Either you used `global_mixdown` while
the user's output is muted — use `pids` instead — or the pids you passed have
already exited. Re-scan.

**Nothing happens at all; no log output.** If you are running a bare executable
rather than a bundled GUI app, the permission call blocks with nowhere to draw a
prompt. Build a proper `.app`.

**`mrec_scan` finds nothing during an obvious call.** Dump what the
detector sees (`mrec_list_audio_processes`) and check the bundle id / exe name
against the catalog in `native/macos/MeetingCatalog.swift` or
`native/windows/MeetingDetector.cpp`. Unlisted apps are not detected.

**Titles and URLs are always empty.** Accessibility is not granted. Note that the
app must be **relaunched** after granting it.

**Audio was fine and is now broken machine-wide.** You (or a crash) killed a
process holding a tap. `sudo killall coreaudiod` — expect a brief glitch.

---

## Status

Verified on macOS 26.5.1, Apple silicon:

- process-tap capture producing real audio **while the system was muted** —
  577,536 samples for exactly 6.0s at 48kHz stereo
- the permission prompt, grant, and resulting TCC entry
- full library path: permission probe → 42 processes enumerated → capture started
  → 48kHz stereo negotiated → 288,768 frames in 6.01s → clean stop
- 30/30 classification assertions; struct offsets asserted against the header at
  build time

Not yet verified:

- **the library's captured samples being non-silent.** The pipeline provably runs
  and produces correctly-timed frames, but the last runs recorded silence — a
  wedged `coreaudiod` from repeated `SIGKILL`s during development, reproduced
  identically by a standalone prototype. Run `sudo killall coreaudiod`, then
  `./scripts/build-macos.sh selftest` and `open native/build/MrecSelfTest.app`.
- **positive meeting detection.** Nothing was in a call during testing. Join one
  and run `./native/build/meetingtest`.
- **both Windows backends.** Written from the reverse-engineered API surface and
  Microsoft's documented contracts; never executed.

### Not built yet

- microphone capture and mic/system mixing
- Opus/WAV encoding
- participant names and mute state
- ScreenCaptureKit fallback for macOS 14.2–14.3
- browser URLs on Windows (needs UI Automation)

---

## Building and testing

```bash
npm run build      # everything: native core, Node addon + TS, Rust
npm test           # native checks + Node tests + cargo test
```

Individually:

```bash
./scripts/build-macos.sh                 # core + layout check + unit tests
npm run build --workspace meeting-record # Node addon + TypeScript
cargo test                               # Rust crate and FFI layout tests
```

Try it interactively:

```bash
# play some audio first, then record 10s to a playable .wav
node node/examples/record-wav.mjs 10

# live detection monitor — open a call and watch it react
node node/examples/watch.mjs

# record every meeting automatically
node node/examples/record.mjs
```

Lower-level checks:

```bash
./native/build/meetingtest           # detection; needs no permission
cat /tmp/meeting-record-detecttest.log

./scripts/build-macos.sh selftest    # signed .app exercising real capture
open native/build/MrecSelfTest.app   # play audio first
cat /tmp/meeting-record-selftest.log
```

`open` does not forward environment variables, so the capture self-test reads
options from files:

```bash
echo -n "$PID" > /tmp/meeting-record-test-pids   # capture one process
touch /tmp/meeting-record-test-global            # capture the whole system mix
```

---

## Notes on consent

The operating system displays no indicator while these APIs are in use. Recording
people requires their consent in many jurisdictions; obtaining it is the
responsibility of the application using this library.
