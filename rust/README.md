# meeting-record for Rust

[![crates.io](https://img.shields.io/crates/v/meeting-record)](https://crates.io/crates/meeting-record) [![docs.rs](https://img.shields.io/docsrs/meeting-record)](https://docs.rs/meeting-record)

System-audio and microphone capture with meeting detection for macOS, Windows, and Linux.
See the [project README](https://github.com/hoangvvo/meeting-record#readme) for requirements, permissions, and supported apps, and the [API reference](https://docs.rs/meeting-record) for types and methods.

```sh
cargo add meeting-record
```

## Usage

Capture system audio for ten seconds.

```rust
use std::{thread, time::Duration};
use meeting_record::{capture, permissions, CaptureOptions, CaptureTarget, Error, Permission};

fn main() -> Result<(), Error> {
    permissions::request_and_wait(Permission::SystemAudio, Duration::from_secs(60), None)?;
    let recording = capture::start(CaptureTarget::System, CaptureOptions::default())?;

    thread::scope(|scope| {
        scope.spawn(|| {
            thread::sleep(Duration::from_secs(10));
            recording.stop();
        });
        while let Some(chunk) = recording.system_audio().recv() {
            println!("{} samples at {} Hz", chunk.frames.len(), chunk.sample_rate);
        }
    });
    Ok(())
}
```

Select an app and include its microphone and a 24 kHz mix.

```rust
use meeting_record::{meetings, MicrophoneSource, MixedAudioOptions};

permissions::request_and_wait(Permission::Microphone, Duration::from_secs(60), None)?;
let app = meetings::audio_apps()?.into_iter().next().expect("No audio apps");

let app_recording = capture::start(
    CaptureTarget::Process { pid: app.pid },
    CaptureOptions {
        microphone: Some(MicrophoneSource::Auto),
        mix: Some(MixedAudioOptions { sample_rate: 24_000 }),
        ..CaptureOptions::default()
    },
)?;
```

Inspect the capture scope and read changes.

```rust
println!("{:?}", app_recording.system_audio().capture_info());
while let Some(event) = app_recording.try_recv_event() {
    println!("{event:?}");
}
```

On Windows, calling apps such as Microsoft Teams, WhatsApp and 8x8 Work use system audio when process capture is unreliable. Set `allow_system_audio_fallback: false` to disable this fallback. System audio can include other apps and recorder playback, and output volume/mute may affect the recording.

Read the mix and pause, resume, or stop the recording.

```rust
thread::scope(|scope| {
    scope.spawn(|| {
        let mixed = app_recording.mixed_audio().expect("mix was requested");
        while let Some(chunk) = mixed.recv() {
            println!("{} bytes, level {}", chunk.pcm.len(), chunk.level);
        }
    });
    app_recording.pause();
    app_recording.resume();
    thread::sleep(Duration::from_secs(10));
    app_recording.stop();
});
```

Scan for meetings or watch for changes.

```rust
let active: Vec<_> = meetings::scan()?
    .into_iter()
    .filter(|meeting| meeting.should_record)
    .collect();

let watcher = meetings::watch(|event, meeting| {
    if event == meeting_record::MeetingEvent::Ended {
        println!("Ended: {:?} {}", meeting.platform, meeting.pid);
    } else {
        println!("{event:?}: {meeting:?}");
    }
})?;
thread::sleep(Duration::from_secs(10));
drop(watcher);
```

On `Ended`, use `platform` and `pid` to remove the meeting. Other payload fields, including `should_record`, may contain the last active observation. Meeting events do not stop capture; call `stop()` and drain the tracks when you want the recording to end.

## License

[MIT](./LICENSE.md) © Hoang Vo
