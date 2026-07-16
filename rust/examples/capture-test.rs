//! Capture whatever is playing for four seconds and report the level.
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use meeting_record::{capture, CaptureOptions, CaptureTarget, Error};

fn main() -> Result<(), Error> {
    println!("capturing the system mix");

    let samples = Arc::new(AtomicUsize::new(0));
    let peak_bits = Arc::new(AtomicU64::new(0));
    let (s, p) = (samples.clone(), peak_bits.clone());

    let capture = Arc::new(capture::start(
        CaptureTarget::System,
        CaptureOptions {
            mono: false,
            ..CaptureOptions::default()
        },
    )?);

    println!(
        "format: {}Hz {}ch",
        capture.system_audio().sample_rate(),
        capture.system_audio().channels()
    );
    let reader = Arc::clone(&capture);
    let worker = thread::spawn(move || {
        while let Some(chunk) = reader.system_audio().recv() {
            s.fetch_add(chunk.frames.len(), Ordering::Relaxed);
            let mut local = 0f32;
            for value in chunk.frames {
                if value.abs() > local {
                    local = value.abs();
                }
            }
            p.fetch_max(local.to_bits() as u64, Ordering::Relaxed);
        }
    });
    thread::sleep(Duration::from_secs(4));
    capture.stop();
    worker.join().unwrap();

    let n = samples.load(Ordering::Relaxed);
    let peak = f32::from_bits(peak_bits.load(Ordering::Relaxed) as u32);
    println!("samples={n} peak={peak:.6}");
    println!(
        "{}",
        if n > 0 && peak > 0.0001 {
            "PASS: real audio captured through Rust"
        } else {
            "FAIL: silent or empty"
        }
    );
    Ok(())
}
