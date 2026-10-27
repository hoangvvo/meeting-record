# meeting-record for Rust

System-audio and microphone capture with meeting detection for macOS and Windows.

## Install

```sh
cargo add meeting-record
```

- See the [platform requirements and permissions](https://github.com/hoangvvo/meeting-record#requirements) for app configuration.
- The [API reference](https://docs.rs/meeting-record) documents the public types and methods.

## Capture

```rust
use std::{thread, time::Duration};
use meeting_record::{capture, permissions, CaptureOptions, CaptureTarget, Error, Permission};

fn main() -> Result<(), Error> {
    permissions::request_and_wait(Permission::SystemAudio, Duration::from_secs(60), None)?;
    let recording = capture::start(CaptureTarget::System, CaptureOptions::default())?;

    thread::scope(|scope| {
        scope.spawn(|| {
            while let Some(chunk) = recording.system_audio().recv() {
                // Raw float32 PCM, with the format and capture time of this packet.
                println!(
                    "{} samples at {} ns, {} Hz, {} channels",
                    chunk.frames.len(), chunk.host_time_ns, chunk.sample_rate, chunk.channels,
                );
            }
        });

        thread::sleep(Duration::from_secs(10));
        recording.stop(); // The reader drains queued audio before the scope ends.
    });
    Ok(())
}
```

| Target                        | Value passed to `capture::start()` |
| ----------------------------- | ---------------------------------- |
| System audio                  | `CaptureTarget::System`            |
| One app and its audio helpers | `CaptureTarget::Process { pid }`   |

- **App picker:** `meetings::audio_apps()` lists available apps. Use the selected entry's `pid`; no meeting scan is required.
- **Meeting capture:** a detected meeting's `pid` is also a process target.
- **Permissions:** `permissions::request_and_wait()` returns a `PermissionStatus`; `permissions::status()` checks the current status without prompting.
- **Sessions:** one recording at a time per process. Keep the `CaptureSession` alive; dropping it stops capture.

### Add a microphone

Request microphone permission, then pass `microphone` in the capture options:

```rust
use meeting_record::MicrophoneSource;

permissions::request_and_wait(Permission::Microphone, Duration::from_secs(60), None)?;
let options = CaptureOptions {
    mono: false, // Stereo system audio; defaults to mono. The mic stays mono.
    microphone: Some(MicrophoneSource::Default),
    ..CaptureOptions::default()
};
let recording = capture::start(CaptureTarget::System, options)?;
```

- `MicrophoneSource::Default` follows the OS input device.
- `MicrophoneSource::Auto` prefers the captured app's mic on Windows, then the communications default. On macOS, it uses the default input.
- Without `microphone`, only system audio is captured.

## Audio output

| Output       | Access                            | Data                                                               |
| ------------ | --------------------------------- | ------------------------------------------------------------------ |
| System audio | `recording.system_audio().recv()` | `Option<AudioChunk>`                                               |
| Microphone   | `recording.microphone()`          | `Option<&AudioTrack>`; call `recv()` on the track                  |
| Mixed audio  | `recording.mixed_audio()`         | `Option<&MixedAudio>`; call `recv()` for `MixedAudioChunk` packets |

- **Raw samples:** `chunk.frames` contains interleaved float32 PCM.
- **Packet metadata:** `host_time_ns`, `frame_count`, `sample_rate`, `channels`, and `dropped_samples`.
- **Reading:** `recv()` blocks until a packet arrives or returns `None` when stopped and drained. Read tracks on worker threads while controlling the session elsewhere.
- **Slow readers:** raw tracks report discarded samples through `dropped_samples` on the track and each packet.
- See [capture formats and channels](https://github.com/hoangvvo/meeting-record#capture) for sample rates and platform behavior.

### Mixed audio

After requesting the audio and microphone permissions, start with `mix` to add a combined stream:

```rust
use meeting_record::MixedAudioOptions;

let options = CaptureOptions {
    microphone: Some(MicrophoneSource::Default),
    mix: Some(MixedAudioOptions { sample_rate: 24_000 }), // Combined mono s16le at 24 kHz.
    ..CaptureOptions::default()
};
let recording = capture::start(CaptureTarget::System, options)?;
```

In the reader thread from the capture example, read the mix instead of `system_audio()`:

```rust
let mixed = recording.mixed_audio().expect("mix was requested");
mixed.set_microphone_muted(true); // Affects the mix only.

while let Some(chunk) = mixed.recv() {
    println!("{} bytes, level {}", chunk.pcm.len(), chunk.level);
}
```

- Mixed packets contain mono signed 16-bit little-endian PCM in `chunk.pcm`.
- Raw tracks remain available alongside the mix.

## Controls and events

| Action                    | API                                                                                      |
| ------------------------- | ---------------------------------------------------------------------------------------- |
| Pause / resume            | `recording.pause()` / `recording.resume()`                                               |
| Stop                      | `recording.stop()`; safe to call more than once                                          |
| Check recording state     | `recording.state()`                                                                      |
| Check source availability | `track.availability()`                                                                   |
| Cancel start or recording | Pass a `CancellationToken` as `CaptureOptions::cancellation`; call `cancel()` on a clone |

- **Pause:** affects the whole recording; paused time is omitted.
- **Stop:** keep reading each output until `None` to drain queued audio.
- **Source loss:** the session stays alive. Track availability is `Live`, `Degraded`, or `Interrupted`.
- **App exit:** system audio stops when all captured apps exit; the microphone and session stay alive. Start a new session for a relaunched app.

Read source events on a separate worker thread alongside the audio reader:

```rust
while let Some(event) = recording.recv_event() {
    println!("{event:?}");
}
```

- `TrackEvent` reports `Interrupted`, `Degraded`, `Resumed`, and `DeviceChanged`.
- Use `try_recv_event()` to poll without blocking.

## Meetings

```rust
use meeting_record::meetings;

let active: Vec<_> = meetings::scan()?
    .into_iter()
    .filter(|meeting| meeting.should_record)
    .collect();

let watcher = meetings::watch(|event, meeting| println!("{event:?}: {meeting:?}"))?;

// Keep the watcher alive; drop it later to stop watching.
```

- Use `should_record` to decide whether a meeting is ready to record.
- The library incorporates call controls and audio signals into `confidence` and `should_record`; no consumer-side call-state logic is required.
- The watcher reports `Started`, `Updated`, and `Ended` events.
- Request `Permission::Accessibility` on macOS for call-state checks, browser detection, titles, and URLs; relaunch after granting it.
- See [supported services and detection behavior](https://github.com/hoangvvo/meeting-record#detection).

## Types and errors

- **Types:** capture targets, platforms, permissions, recording states, and source events use enums.
- **Events:** `TrackEvent` and `InterruptionReason` are non-exhaustive; include a fallback arm when matching them.
- **Library failures:** capture, permissions, and enumeration return `Result<_, Error>`.
- **Cancellation:** cancelled operations return `Error::Cancelled`; cancelling an active recording stops capture.

## License

[MIT](./LICENSE.md) © Hoang Vo
