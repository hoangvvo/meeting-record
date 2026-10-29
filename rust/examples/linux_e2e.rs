//! Run with e2e/linux.sh through MREC_RUST_E2E.
use meeting_record::{capture, CaptureOptions, CaptureTarget, MicrophoneSource, MixedAudioOptions};
use std::{env, thread, time::Duration};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let pid = env::args().nth(1).expect("target pid").parse()?;
    let session = capture::start(
        CaptureTarget::Process { pid },
        CaptureOptions {
            microphone: Some(MicrophoneSource::Default),
            mix: Some(MixedAudioOptions { sample_rate: 16000 }),
            ..Default::default()
        },
    )?;
    thread::sleep(Duration::from_millis(1200));
    session.stop();
    let mut frames = 0;
    let mut last = 0;
    let mut samples = Vec::new();
    while let Some(chunk) = session.system_audio().recv() {
        assert_eq!(chunk.channels, 1);
        assert_eq!(chunk.sample_rate, 48000.0);
        assert_eq!(chunk.dropped_samples, 0);
        assert!(chunk.host_time_ns >= last);
        last = chunk.host_time_ns;
        frames += chunk.frame_count;
        samples.extend(chunk.frames);
    }
    assert!(frames > 30000, "missing system audio");
    let power = |frequency: f64| {
        let (mut re, mut im, mut energy) = (0.0, 0.0, 0.0);
        for (index, &sample) in samples.iter().enumerate() {
            let angle = 2.0 * std::f64::consts::PI * frequency * index as f64 / 48000.0;
            re += sample as f64 * angle.cos();
            im += sample as f64 * angle.sin();
            energy += (sample as f64).powi(2);
        }
        2.0 * (re * re + im * im) / samples.len() as f64 / energy.max(1e-12)
    };
    assert!(power(880.0) > 0.5, "target tone missing");
    assert!(power(990.0) < 0.01, "unrelated tone leaked");
    let mut microphone_frames = 0;
    while let Some(chunk) = session.microphone().unwrap().recv() {
        microphone_frames += chunk.frame_count;
    }
    assert!(microphone_frames > 30000, "missing microphone");
    assert!(!capture::running());
    println!(
        "Rust Linux live capture, separate tracks, isolation, timestamps, and stop tail passed"
    );
    Ok(())
}
