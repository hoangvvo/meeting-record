//! Record every meeting to a raw float32 file.
//!
//! The audio callback runs on a realtime thread, so it only forwards into a
//! channel; the writing happens on a normal thread.

use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::mpsc;

fn main() -> Result<(), meeting_record::Error> {
    if meeting_record::audio_permission() != meeting_record::Permission::Granted {
        meeting_record::request_audio_permission();
        eprintln!("grant system audio recording, then rerun");
        return Ok(());
    }

    let (tx, rx) = mpsc::channel::<Vec<f32>>();
    std::thread::spawn(move || {
        let mut out = BufWriter::new(File::create("meeting.f32").unwrap());
        while let Ok(chunk) = rx.recv() {
            for sample in chunk {
                out.write_all(&sample.to_le_bytes()).unwrap();
            }
        }
    });

    let _watcher = meeting_record::watch(move |event, meeting| {
        if event == meeting_record::MeetingEvent::Started && meeting.should_record {
            println!("recording {} ({})", meeting.platform.as_str(), meeting.app_name);
            let tx = tx.clone();
            // Realtime thread: copy and send, nothing else.
            match meeting_record::record(meeting, move |buffer| {
                let _ = tx.send(buffer.frames.to_vec());
            }) {
                Ok(capture) => {
                    println!("  {}Hz {}ch", capture.sample_rate, capture.channels);
                    // Leaked deliberately: dropping the guard would stop capture.
                    std::mem::forget(capture);
                }
                Err(e) => eprintln!("  failed: {e}"),
            }
        }
    })?;

    println!("watching for meetings; ctrl-c to stop");
    std::thread::sleep(std::time::Duration::from_secs(3600));
    Ok(())
}
