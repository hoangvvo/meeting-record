# meeting-record for Rust

[Full documentation](https://github.com/hoangvvo/meeting-record#readme)

## Install

```sh
cargo add meeting-record
```

## Usage

```rust
use std::thread;
use std::time::Duration;

use meeting_record::{
    capture, meetings, permissions, CaptureOptions, CaptureTarget, Error, Permission,
    PermissionStatus,
};

fn main() -> Result<(), Error> {
    if permissions::status(Permission::SystemAudio) != PermissionStatus::Granted {
        permissions::request(Permission::SystemAudio)?;
        return Ok(());
    }

    let _watcher = meetings::watch(|event, meeting| println!("{event:?}: {meeting:?}"))?;

    let Some(meeting) = meetings::scan()
        .into_iter()
        .find(|meeting| meeting.should_record)
    else {
        return Ok(());
    };

    let recording = capture::start(
        CaptureTarget::Process { pid: meeting.pid },
        CaptureOptions::default(),
    )?;
    thread::scope(|scope| {
        scope.spawn(|| {
            while let Some(chunk) = recording.system_audio().recv() {
                println!("{} samples", chunk.frames.len());
            }
        });
        thread::sleep(Duration::from_secs(10));
        recording.stop();
    });
    Ok(())
}
```

## License

[MIT](./LICENSE.md) © Hoang Vo
