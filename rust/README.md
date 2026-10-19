# meeting-record for Rust

[API documentation](https://docs.rs/meeting-record) · [Project documentation](https://github.com/hoangvvo/meeting-record#readme)

System-audio and microphone capture with meeting detection for macOS and Windows.

## Platform support

| Platform | Minimum version | Architectures |
| -------- | --------------- | ------------- |
| macOS    | 14.2            | arm64, x86-64 |
| Windows  | 10 build 20348  | arm64, x86-64 |

## Install

```sh
cargo add meeting-record
```

## Usage

```rust
use std::thread;
use std::time::Duration;

use meeting_record::{
    capture, meetings, permissions, CaptureOptions, CaptureState, CaptureTarget, Error,
    MicrophoneSource, Permission, PermissionStatus,
};

fn permission_ready(permission: Permission) -> bool {
    matches!(
        permissions::status(permission),
        PermissionStatus::Granted | PermissionStatus::NotRequired
    )
}

fn main() -> Result<(), Error> {
    // Audio permission requests return immediately; rerun after granting them.
    for (permission, name) in [
        (Permission::SystemAudio, "system audio"),
        (Permission::Microphone, "microphone"),
    ] {
        if !permission_ready(permission) {
            permissions::request(permission)?;
            eprintln!("grant {name} access, then rerun");
            return Ok(());
        }
    }

    // Request accessibility permission to read meeting titles and URLs for detection.
    if !permission_ready(Permission::Accessibility) {
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

        scope.spawn(|| {
            let mut health = recording.health();
            while recording.state() != CaptureState::Stopped {
                let next = recording.health();
                if next != health {
                    println!("capture health: {next:?}");
                    health = next;
                }
                thread::sleep(Duration::from_millis(250));
            }
            if let Some(error) = recording.failure() {
                eprintln!("capture failed: {error}");
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

`meetings::audio_apps()` lists owning applications for a source picker. Use an entry's `pid` with `CaptureTarget::Process`; no meeting scan or Accessibility grant is needed.

Windows targets include future child processes. macOS targets include current audio helpers; restart capture if helpers are replaced.

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

`mixed_audio()` is mono s16le. Pause/resume excludes paused time; drain it after stopping to consume the final tail. Raw tracks remain float32. Mixing does not perform echo cancellation.

Set `CaptureOptions::cancellation` to a cloned `CancellationToken` to cancel capture. Use `permissions::request_and_wait(permission, timeout, Some(&token))` for cancellable permission waits.

## License

[MIT](./LICENSE.md) © Hoang Vo
