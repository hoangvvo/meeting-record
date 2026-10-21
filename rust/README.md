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

    let Some(meeting) = meetings::scan()
        .into_iter()
        .find(|meeting| meeting.should_record)
    else {
        return Ok(());
    };

    let recording = capture::start(
        CaptureTarget::Process { pid: meeting.pid },
        CaptureOptions {
            microphone: Some(MicrophoneSource::Default),
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

        // A lost source never stops the recording; its track resumes when it returns.
        scope.spawn(|| {
            while let Some(event) = recording.recv_event() {
                match event {
                    TrackEvent::Interrupted { track, reason, .. } => {
                        eprintln!("{track:?} interrupted: {reason:?}")
                    }
                    TrackEvent::Resumed { track } => println!("{track:?} resumed"),
                    TrackEvent::DeviceChanged { device_name, .. } => {
                        println!("microphone: {device_name}")
                    }
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
- Windows: the target includes future child processes.
- macOS: the target includes current audio helpers; restart capture if they change.

## Mixed output

```rust
use std::io::Write;
use meeting_record::{capture, CaptureOptions, CaptureTarget, MicrophoneSource, MixedAudioOptions};

let recording = capture::start(CaptureTarget::System, CaptureOptions {
    microphone: Some(MicrophoneSource::Default),
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
