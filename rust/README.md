# meeting-record for Rust

[API documentation](https://docs.rs/meeting-record) · [Project documentation](https://github.com/hoangvvo/meeting-record#readme)

System-audio and microphone capture with meeting detection for macOS and Windows.

## Platform support

| Platform | Minimum version | Architectures |
| -------- | --------------- | ------------- |
| macOS    | 14.2            | arm64, x86-64 |
| Windows  | 10 build 19041  | arm64, x86-64 |

## Install

```sh
cargo add meeting-record
```

## Usage

```rust
use std::thread;
use std::time::Duration;

use meeting_record::{
    capture, meetings, permissions, CaptureOptions, CaptureTarget, Error, MicrophoneSource,
    Permission, PermissionStatus, TrackEvent,
};

fn main() -> Result<(), Error> {
    // Audio permission requests return immediately; rerun after granting them.
    for (permission, name) in [
        (Permission::SystemAudio, "system audio"),
        (Permission::Microphone, "microphone"),
    ] {
        if permissions::status(permission) != PermissionStatus::Granted {
            permissions::request(permission)?;
            eprintln!("grant {name} access, then rerun");
            return Ok(());
        }
    }

    // Request accessibility permission to read meeting titles and URLs for detection.
    if permissions::status(Permission::Accessibility) != PermissionStatus::Granted {
        permissions::request(Permission::Accessibility)?;
    }

    // Watch for meetings starting, changing, and ending.
    let watcher = meetings::watch(|event, meeting| println!("{event:?}: {meeting:?}"))?;

    // Or scan for active meetings at any time.

    let Some(meeting) = meetings::scan()?
        .into_iter()
        .find(|meeting| meeting.should_record)
    else {
        return Ok(());
    };

    let recording = capture::start(
        CaptureTarget::Process { pid: meeting.pid },
        CaptureOptions {
            microphone: Some(MicrophoneSource::Auto),
            ..CaptureOptions::default()
        },
    )?;
    thread::scope(|scope| {
        scope.spawn(|| {
            while let Some(chunk) = recording.system_audio().recv() {
                println!(
                    "{} samples at {} ns ({} Hz, {} channels, {} dropped)",
                    chunk.frames.len(),
                    chunk.host_time_ns,
                    chunk.sample_rate,
                    chunk.channels,
                    chunk.dropped_samples,
                );
            }
        });

        // Lost devices recover without ending the session; exited apps need a new target.
        scope.spawn(|| {
            while let Some(event) = recording.recv_event() {
                match event {
                    TrackEvent::Interrupted { track, reason, .. } => {
                        eprintln!("{track:?} interrupted: {reason:?}")
                    }
                    TrackEvent::Resumed { track, .. } => println!("{track:?} resumed"),
                    TrackEvent::DeviceChanged { device_name, .. } => {
                        println!("microphone: {device_name}")
                    }
                    TrackEvent::Degraded { track, reason, .. } => {
                        eprintln!("{track:?} partially unavailable: {reason:?}")
                    }
                    _ => {}
                }
            }
        });

        thread::sleep(Duration::from_secs(10));
        recording.stop();
    });

    // Stop watching for meetings when done.
    drop(watcher);
    Ok(())
}
```

## Audio apps

- `meetings::audio_apps()`: applications doing audio IO, for a source picker.
- Use an entry's `pid` with `CaptureTarget::Process`; no meeting scan or Accessibility grant needed.
- Windows follows future child processes; macOS refreshes audio helpers about once a second.
- A relaunched app requires a new capture session.

## Mixed output

```rust
use std::io::Write;
use meeting_record::{capture, CaptureOptions, CaptureTarget, MicrophoneSource, MixedAudioOptions};

let recording = capture::start(CaptureTarget::System, CaptureOptions {
    microphone: Some(MicrophoneSource::Auto),
    mix: Some(MixedAudioOptions { sample_rate: 24_000 }),
    ..CaptureOptions::default()
})?;
let mixed = recording.mixed_audio().unwrap();
mixed.set_microphone_muted(true);
while let Some(chunk) = mixed.recv() {
    destination.write_all(&chunk.pcm)?;
    println!("{}", chunk.level);
}
```

- `mixed_audio()`: mono s16le; raw tracks stay float32.
- Paused time is excluded.
- After stopping, drain it to consume the final tail.
- No echo cancellation.
- `CaptureOptions::cancellation`: a cloned `CancellationToken` stops capture.
- A lost source pauses only its own track in the mix.
- `permissions::request_and_wait(permission, timeout, Some(&token))`: cancellable permission wait.

## License

[MIT](./LICENSE.md) © Hoang Vo

## Availability and compatibility

`AudioTrack::availability()` distinguishes `Live`, `Degraded` (some sources missing),
and `Interrupted` (none available). `is_interrupted()` is true only for the latter.
Availability is independent of pause/stop; after stop it is the last observed state.
Stopping preserves captured packets, including the native backend's final flush.
Drain outputs until `None` to consume the tail.

`MicrophoneSource::Default` follows the OS default input. `Auto` prefers the captured
app's microphone on Windows, then the communications default; macOS follows its
system default input. Explicitly requested sources must be available at startup;
later device loss recovers independently while the session remains alive.

Mixed output omits intervals over one second in which every input was absent;
a missing input contributes silence while another track continues. Raw packets
retain their original host timestamps for wall-clock alignment.

Changes from 0.1.1: `meetings::scan()`, `audio_apps()`, and `audio_processes()` return
`Result` so enumeration errors stay distinguishable from an empty list. Use `?` to
propagate failures. Use `MicrophoneSource::Auto` for the previous Windows selection
policy. Match public track events with a fallback arm for future event types.
