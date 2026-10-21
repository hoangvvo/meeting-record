# meeting-record for Rust

System-audio and microphone capture with meeting detection for macOS and Windows.
See the [API docs](https://docs.rs/meeting-record) and
[platform requirements](https://github.com/hoangvvo/meeting-record#requirements).

```sh
cargo add meeting-record
```

## Usage

```rust
use std::{thread, time::Duration};
use meeting_record::{capture, permissions, CaptureOptions, CaptureTarget, Error, Permission};

fn main() -> Result<(), Error> {
    permissions::request_and_wait(Permission::SystemAudio, Duration::from_secs(60), None)?;
    let recording = capture::start(CaptureTarget::System, CaptureOptions::default())?;

    thread::scope(|scope| {
        // Read audio while the main thread controls the recording.
        scope.spawn(|| {
            while let Some(chunk) = recording.system_audio().recv() {
                println!("{} samples at {} ns", chunk.frames.len(), chunk.host_time_ns);
            }
        });
        thread::sleep(Duration::from_secs(10));
        // The reader drains queued audio before leaving the scope.
        recording.stop();
    });
    Ok(())
}
```

- **App picker:** `meetings::audio_apps()` lists available apps.
- **Process capture:** `CaptureTarget::Process { pid }` captures the app and its audio helpers.
- **Microphone:** optional; request `Permission::Microphone` before including it.

```rust
use meeting_record::MicrophoneSource;

let options = CaptureOptions {
    mono: false, // Stereo system audio; defaults to mono. The mic stays mono.
    microphone: Some(MicrophoneSource::Default), // Follow the OS input device.
    ..CaptureOptions::default()
};
```

Use `MicrophoneSource::Auto` to prefer the captured app's mic on Windows, then the
communications default. On macOS, both modes use the default input.

## Meetings

```rust
use meeting_record::meetings;

let active: Vec<_> = meetings::scan()?
    .into_iter()
    .filter(|meeting| meeting.should_record)
    .collect();
let watcher = meetings::watch(|event, meeting| println!("{event:?}: {meeting:?}"))?;

// Drop the watcher to stop watching.
```

- The watcher reports started, updated, and ended meetings.
- Request `Permission::Accessibility` on macOS to include titles and URLs.
- Enumeration functions return `Result`.

## Audio output

Raw tracks yield float32 samples with capture timestamps, sample rate, and channel
count. Optional mixed output is available through `recording.mixed_audio()`.

```rust
use meeting_record::MixedAudioOptions;

let options = CaptureOptions {
    mix: Some(MixedAudioOptions { sample_rate: 24_000 }), // Combined mono s16le at 24 kHz.
    ..CaptureOptions::default()
};
```

- **Pause/resume:** `pause()` and `resume()` control capture.
- **Stop:** `stop()` preserves queued audio. Drain outputs until `None`; dropping the session also stops capture.
- **Cancellation:** `CaptureOptions::cancellation` accepts a `CancellationToken`.
- **Source loss:** the session stays alive; `recv_event()` reports partial loss, interruption, recovery, and mic device changes.
- **Events:** match `TrackEvent` with a fallback arm for future events.
- **App exit:** a relaunched app requires a new capture session.

## License

[MIT](./LICENSE.md) © Hoang Vo
