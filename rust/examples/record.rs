//! Record every meeting to a raw float32 file.
//!
//! Track data is drained on a writer thread so meeting detection stays responsive.

use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use meeting_record::{
    capture, meetings, permissions, AudioTrack, CaptureOptions, CaptureTarget, Error, MeetingEvent,
    MicrophoneSource, Permission, PermissionStatus,
};

fn write_track(track: &AudioTrack, path: &str) {
    let mut output = BufWriter::new(File::create(path).unwrap());
    while let Some(chunk) = track.recv() {
        for sample in chunk.frames {
            output.write_all(&sample.to_le_bytes()).unwrap();
        }
    }
}

fn main() -> Result<(), Error> {
    if permissions::status(Permission::SystemAudio) != PermissionStatus::Granted {
        permissions::request(Permission::SystemAudio)?;
        eprintln!("grant system audio recording, then rerun");
        return Ok(());
    }
    if permissions::status(Permission::Microphone) != PermissionStatus::Granted {
        permissions::request(Permission::Microphone)?;
        eprintln!("grant microphone access, then rerun");
        return Ok(());
    }

    let _watcher = meetings::watch(move |event, meeting| {
        if event == MeetingEvent::Started && meeting.should_record {
            println!(
                "recording {} ({})",
                meeting.platform.as_str(),
                meeting.app_name
            );
            match capture::start(
                CaptureTarget::Process { pid: meeting.pid },
                CaptureOptions {
                    microphone: Some(MicrophoneSource::Default),
                    ..CaptureOptions::default()
                },
            ) {
                Ok(recording) => {
                    let recording = Arc::new(recording);
                    println!(
                        "  system: {}Hz {}ch",
                        recording.system_audio().sample_rate(),
                        recording.system_audio().channels()
                    );
                    let microphone = recording.microphone().expect("microphone was requested");
                    println!(
                        "  microphone: {}Hz {}ch",
                        microphone.sample_rate(),
                        microphone.channels()
                    );

                    let system_path = format!("{}-system.f32", meeting.platform.as_str());
                    let system_recording = Arc::clone(&recording);
                    thread::spawn(move || {
                        write_track(system_recording.system_audio(), &system_path);
                    });

                    let microphone_path = format!("{}-microphone.f32", meeting.platform.as_str());
                    let microphone_recording = Arc::clone(&recording);
                    thread::spawn(move || {
                        write_track(
                            microphone_recording
                                .microphone()
                                .expect("microphone was requested"),
                            &microphone_path,
                        );
                    });
                }
                Err(e) => eprintln!("  failed: {e}"),
            }
        }
    })?;

    println!("watching for meetings; ctrl-c to stop");
    thread::sleep(Duration::from_secs(3600));
    Ok(())
}
