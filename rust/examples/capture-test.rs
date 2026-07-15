//! Capture whatever is playing for four seconds and report the level.
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;

fn main() -> Result<(), meeting_record::Error> {
    let playing: Vec<u32> = meeting_record::audio_processes()
        .iter()
        .filter(|p| p.is_playing_audio)
        .map(|p| p.pid)
        .collect();
    println!("capturing pids: {playing:?}");

    let samples = Arc::new(AtomicUsize::new(0));
    let peak_bits = Arc::new(AtomicU64::new(0));
    let (s, p) = (samples.clone(), peak_bits.clone());

    let capture = meeting_record::capture(
        meeting_record::CaptureOptions {
            pids: playing,
            stereo: true,
            ..Default::default()
        },
        move |buffer| {
            s.fetch_add(buffer.frames.len(), Ordering::Relaxed);
            let mut local = 0f32;
            for &v in buffer.frames {
                if v.abs() > local {
                    local = v.abs();
                }
            }
            let bits = local.to_bits() as u64;
            p.fetch_max(bits, Ordering::Relaxed);
        },
    )?;

    println!("format: {}Hz {}ch", capture.sample_rate, capture.channels);
    std::thread::sleep(std::time::Duration::from_secs(4));
    drop(capture); // stops capture

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
