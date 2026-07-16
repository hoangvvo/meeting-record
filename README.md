# meeting-record

System-audio and microphone capture with meeting detection for macOS (14.2+) and Windows (10 build 20348+). Records call audio without virtual audio devices, kernel extensions, or screen recording.

## Installation

```bash
npm install meeting-record     # TypeScript / Node
cargo add meeting-record       # Rust

```

For C, build the static library (`./scripts/build-macos.sh` or compile `native/windows/*.cpp`) and link it with the necessary frameworks/libraries (e.g., `CoreAudio`, `AVFoundation`, `ole32`).

**macOS requirement:** System-audio capture needs `NSAudioCaptureUsageDescription`. Add `NSMicrophoneUsageDescription` only when requesting the microphone track. The app must also be signed and running a GUI (`NSApplication`) for permission prompts.

```xml
<key>NSAudioCaptureUsageDescription</key>
<string>Records meeting audio so it can be transcribed.</string>
<key>NSMicrophoneUsageDescription</key>
<string>Records your voice so meetings can be transcribed.</string>

```

## Quickstart

### TypeScript

```ts
import { createWriteStream } from "node:fs";
import * as MeetingRecord from "meeting-record";

await MeetingRecord.permissions.request("system-audio");
await MeetingRecord.permissions.request("microphone");

const meeting = MeetingRecord.meetings.scan().find((candidate) => candidate.shouldRecord);
if (!meeting) throw new Error("no active meeting");

const recording = await MeetingRecord.capture.start(
  { type: "process", pid: meeting.pid },
  { microphone: "default" },
);
if (!recording.microphone) throw new Error("microphone track was requested");
recording.systemAudio.pipe(createWriteStream(`${meeting.platform}-system.f32`));
recording.microphone.pipe(createWriteStream(`${meeting.platform}-microphone.f32`));

recording.pauseRecording();
recording.resumeRecording();
await recording.stopRecording();
```

### Rust

```rust
use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::Arc;
use std::thread;

use meeting_record::{capture, meetings, permissions, AudioTrack, CaptureOptions, CaptureTarget, MicrophoneSource, Permission, PermissionStatus};

fn write_track(track: &AudioTrack, path: &str) {
    let mut output = BufWriter::new(File::create(path).unwrap());
    while let Some(chunk) = track.recv() {
        for sample in chunk.frames {
            output.write_all(&sample.to_le_bytes()).unwrap();
        }
    }
}

if permissions::status(Permission::SystemAudio) != PermissionStatus::Granted {
    permissions::request(Permission::SystemAudio)?;
    return Ok(());
}
if permissions::status(Permission::Microphone) != PermissionStatus::Granted {
    permissions::request(Permission::Microphone)?;
    return Ok(());
}

let Some(meeting) = meetings::scan()
    .into_iter()
    .find(|meeting| meeting.should_record) else {
    return Ok(());
};

let recording = Arc::new(capture::start(
    CaptureTarget::Process { pid: meeting.pid },
    CaptureOptions {
        microphone: Some(MicrophoneSource::Default),
        ..CaptureOptions::default()
    },
)?);

let system_path = format!("{}-system.f32", meeting.platform.as_str());
let system_recording = Arc::clone(&recording);
let system_writer = thread::spawn(move || {
    write_track(system_recording.system_audio(), &system_path);
});

let microphone_path = format!("{}-microphone.f32", meeting.platform.as_str());
let microphone_recording = Arc::clone(&recording);
let microphone_writer = thread::spawn(move || {
    write_track(
        microphone_recording.microphone().expect("microphone was requested"),
        &microphone_path,
    );
});

recording.pause();
recording.resume();
recording.stop();
system_writer.join().unwrap();
microphone_writer.join().unwrap();

```

Both examples write the same two raw interleaved float32 tracks. Rust receiver threads are the direct equivalent of Node stream pipes.

### C

```c
#include "meeting-record.h"
#include "meeting-record-detect.h"
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

// Realtime callback: no locks, malloc, I/O, or GC.
static void on_audio(const float *frames, uint32_t count, uint32_t channels, double rate, uint64_t time, void *ud) {
  // push to a lock-free ring buffer and bail
}

static void on_meeting(const mrec_meeting *m, mrec_event event, void *ud) {
  if (event == MREC_MEETING_STARTED && m->should_record) {
    mrec_config cfg;
    mrec_config_defaults(&cfg);
    cfg.pids = m->audio_pids;
    cfg.pid_count = m->audio_pid_count;

    // Can block for ~6s on first run. Don't do this on the UI thread.
    // (FFI users: call mrec_start_raw() instead)
    if (mrec_start(&cfg, on_audio, NULL) != MREC_OK) {
      // Grab error immediately (it's a shared global slot)
      fprintf(stderr, "start failed: %s\n", mrec_last_error());
    }
  } else if (event == MREC_MEETING_ENDED) {
    mrec_stop();
  }
}

// Trap signals so we don't exit mid-capture.
// Hard-killing without mrec_stop() leaks audio state and wedges coreaudiod.
static void handle_shutdown(int sig) {
  mrec_stop();
  exit(0);
}

int main(void) {
  signal(SIGINT, handle_shutdown);
  signal(SIGTERM, handle_shutdown);

  mrec_request_audio_permission();

  // Singleton: only one watcher allowed per process
  mrec_watch_start(on_meeting, NULL);

  // ... run loop ...
}

```

## Permissions

| Permission        | Purpose                                                  | Prompt Type               |
| ----------------- | -------------------------------------------------------- | ------------------------- |
| **System Audio**  | Required to capture audio.                               | One-time modal dialog.    |
| **Microphone**    | Required only when the microphone track is requested.    | One-time modal dialog.    |
| **Accessibility** | Optional. Required only to read meeting titles and URLs. | System Settings redirect. |

`mrec_request_audio_permission()` returns immediately while the OS prompts the user. Poll `mrec_audio_permission_status()` to check the result.

Microphone capture is opt-in. It uses the current default input device and stays separate from `systemAudio` / `system_audio`; the library never mixes the two tracks together.

## Detection & Capture

Watch for meetings using event-driven callbacks (`mrec_watch_start`). The watcher runs on a safe serial queue.

**Confidence & `should_record`:**
Always rely on the `should_record` flag. A meeting triggers `should_record = 1` when there is active two-way audio or remote audio. Idle conferencing apps return `0`.

**Process Targeting vs. Global Mixdown:**

- **Targeted (`cfg.pids`):** Recommended. Captures the specific meeting processes. Continues capturing even if the user mutes their system speakers.
- **Global (`cfg.global_mixdown = 1`):** Captures the whole system, but records silence if the system is muted.

## Troubleshooting

- **`mrec_start` returns `-2` (Timeout):** The permission grant is undetermined. Ensure you requested permissions from a bundled GUI app, not a bare CLI executable.
- **Capture runs but outputs zeroes:** You likely used `global_mixdown` while the user's speakers were muted, or the targeted PIDs have exited.
- **Titles/URLs are empty:** Accessibility permission was not granted (or the app wasn't restarted after granting it).
- **Audio is broken machine-wide (macOS):** A crash or `SIGKILL` orphaned a process tap. Run `sudo killall coreaudiod` in your terminal to reset it.
