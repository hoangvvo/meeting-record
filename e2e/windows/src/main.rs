//! Windows live-audio E2E harness.
//!
//! Environment variables:
//! - `MREC_TARGET_PID`: capture this process tree instead of system audio.
//! - `MREC_DURATION_SECONDS`: capture duration (default: 6).
//! - `MREC_MICROPHONE=1`: require a non-silent default-microphone track.
//! - `MREC_EXPECT_RECOVERY=1`: require a recovering -> running transition.
//! - `MREC_MIN_PEAK`: minimum absolute sample peak (default: 0.0001).
//! - `MREC_ALLOW_DROPS=1`: permit bounded-queue drops.

use std::{
    env,
    error::Error as StdError,
    thread,
    time::{Duration, Instant},
};

use meeting_record::{capture, CaptureHealth, CaptureOptions, CaptureTarget, MicrophoneSource};

#[derive(Default)]
struct TrackMetrics {
    frames: u64,
    samples: usize,
    seconds: f64,
    peak: f32,
    dropped_samples: usize,
    malformed_chunks: usize,
    timestamp_regressions: usize,
    first_host_time_ns: Option<u64>,
    last_host_time_ns: Option<u64>,
}

impl TrackMetrics {
    fn add(&mut self, chunk: meeting_record::AudioChunk) {
        let expected_samples = chunk.frame_count as usize * chunk.channels as usize;
        if chunk.frames.len() != expected_samples || chunk.channels == 0 || chunk.sample_rate <= 0.0
        {
            self.malformed_chunks += 1;
        }
        if let Some(previous) = self.last_host_time_ns {
            if chunk.host_time_ns < previous {
                self.timestamp_regressions += 1;
            }
        } else {
            self.first_host_time_ns = Some(chunk.host_time_ns);
        }
        self.last_host_time_ns = Some(chunk.host_time_ns);
        self.frames += u64::from(chunk.frame_count);
        self.samples += chunk.frames.len();
        self.seconds += f64::from(chunk.frame_count) / chunk.sample_rate;
        self.dropped_samples += chunk.dropped_samples;
        for sample in chunk.frames {
            self.peak = self.peak.max(sample.abs());
        }
    }

    fn verify(
        &self,
        name: &str,
        requested_duration: Duration,
        minimum_peak: f32,
        allow_drops: bool,
    ) -> Result<(), String> {
        let minimum_seconds = requested_duration.as_secs_f64() * 0.7;
        if self.frames == 0 || self.seconds < minimum_seconds {
            return Err(format!(
                "{name}: captured only {:.2}s ({}) frames; expected at least {:.2}s",
                self.seconds, self.frames, minimum_seconds
            ));
        }
        if self.peak < minimum_peak {
            return Err(format!(
                "{name}: peak {:.6} is below required {:.6}",
                self.peak, minimum_peak
            ));
        }
        if self.first_host_time_ns == Some(0) {
            return Err(format!("{name}: first host timestamp is zero"));
        }
        if self.malformed_chunks != 0 {
            return Err(format!(
                "{name}: {} chunks had invalid frame/format metadata",
                self.malformed_chunks
            ));
        }
        if self.timestamp_regressions != 0 {
            return Err(format!(
                "{name}: {} timestamp regressions",
                self.timestamp_regressions
            ));
        }
        if !allow_drops && self.dropped_samples != 0 {
            return Err(format!(
                "{name}: {} samples were dropped",
                self.dropped_samples
            ));
        }
        Ok(())
    }

    fn report(&self, name: &str) {
        println!(
            "{name}: {:.2}s, {} frames, {} samples, peak={:.6}, dropped={}, timestamps={}..{}",
            self.seconds,
            self.frames,
            self.samples,
            self.peak,
            self.dropped_samples,
            self.first_host_time_ns.unwrap_or(0),
            self.last_host_time_ns.unwrap_or(0),
        );
    }
}

fn enabled(name: &str) -> bool {
    env::var(name).is_ok_and(|value| matches!(value.as_str(), "1" | "true" | "yes"))
}

fn main() -> Result<(), Box<dyn StdError>> {
    let duration = Duration::from_secs(
        env::var("MREC_DURATION_SECONDS")
            .ok()
            .map_or(Ok(6), |value| value.parse())?,
    );
    let minimum_peak = env::var("MREC_MIN_PEAK")
        .ok()
        .map_or(Ok(0.0001), |value| value.parse())?;
    let target = match env::var("MREC_TARGET_PID") {
        Ok(value) => CaptureTarget::Process {
            pid: value.parse()?,
        },
        Err(_) => CaptureTarget::System,
    };
    let include_microphone = enabled("MREC_MICROPHONE");
    let expect_recovery = enabled("MREC_EXPECT_RECOVERY");
    let allow_drops = enabled("MREC_ALLOW_DROPS");

    println!("starting {target:?} capture for {}s", duration.as_secs());
    let recording = std::sync::Arc::new(capture::start(
        target,
        CaptureOptions {
            mono: false,
            microphone: include_microphone.then_some(MicrophoneSource::Default),
        },
    )?);

    let system_reader = std::sync::Arc::clone(&recording);
    let system_worker = thread::spawn(move || {
        let mut metrics = TrackMetrics::default();
        while let Some(chunk) = system_reader.system_audio().recv() {
            metrics.add(chunk);
        }
        metrics
    });
    let microphone_worker = include_microphone.then(|| {
        let microphone_reader = std::sync::Arc::clone(&recording);
        thread::spawn(move || {
            let mut metrics = TrackMetrics::default();
            let track = microphone_reader
                .microphone()
                .expect("microphone track requested but missing");
            while let Some(chunk) = track.recv() {
                metrics.add(chunk);
            }
            metrics
        })
    });

    let deadline = Instant::now() + duration;
    let mut saw_recovering = false;
    let mut saw_recovered = false;
    let mut previous_health = recording.health();
    while Instant::now() < deadline {
        thread::sleep(Duration::from_millis(50));
        let health = recording.health();
        saw_recovering |= health == CaptureHealth::Recovering;
        saw_recovered |=
            previous_health == CaptureHealth::Recovering && health == CaptureHealth::Running;
        previous_health = health;
        if health == CaptureHealth::Failed {
            break;
        }
    }
    let terminal_failure = recording.failure();
    recording.stop();

    let system = system_worker.join().map_err(|_| "system reader panicked")?;
    let microphone = microphone_worker
        .map(|worker| worker.join().map_err(|_| "microphone reader panicked"))
        .transpose()?;

    if let Some(error) = terminal_failure {
        return Err(error.into());
    }
    if expect_recovery && (!saw_recovering || !saw_recovered) {
        return Err("expected a complete recovering -> running transition".into());
    }

    system.report("system");
    system.verify("system", duration, minimum_peak, allow_drops)?;
    if let Some(microphone) = microphone {
        microphone.report("microphone");
        microphone.verify("microphone", duration, minimum_peak, allow_drops)?;
    }
    println!("PASS: live audio capture met all E2E assertions");
    Ok(())
}
