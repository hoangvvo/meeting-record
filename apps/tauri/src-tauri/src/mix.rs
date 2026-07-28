//! Offline mixdown of the two capture stems into one mono file.
//!
//! Streams the stems rather than loading them: an hour of 48 kHz audio is ~700
//! MB as f32, and meetings run that long. Costs a second read of each stem,
//! because the peak has to be known before any sample can be scaled.

use std::path::Path;

use hound::{SampleFormat, WavReader, WavSpec, WavWriter};

/// Sequential mono reader. Multi-channel stems are averaged down as they
/// arrive.
struct MonoReader {
    samples: Box<dyn Iterator<Item = Result<f32, hound::Error>>>,
    channels: usize,
}

impl MonoReader {
    fn open(path: &Path) -> Result<(Self, f64, u32), String> {
        let reader = WavReader::open(path).map_err(|error| error.to_string())?;
        let spec = reader.spec();
        let frames = reader.duration();
        let samples: Box<dyn Iterator<Item = Result<f32, hound::Error>>> = match spec.sample_format
        {
            SampleFormat::Int => {
                let scale = 1.0 / f32::from(i16::MAX);
                Box::new(
                    reader
                        .into_samples::<i16>()
                        .map(move |sample| sample.map(|value| f32::from(value) * scale)),
                )
            }
            SampleFormat::Float => Box::new(reader.into_samples::<f32>()),
        };
        let reader = Self {
            samples,
            channels: spec.channels.max(1) as usize,
        };
        Ok((reader, f64::from(spec.sample_rate), frames))
    }

    fn next_frame(&mut self) -> Option<f32> {
        let mut sum = 0.0;
        for channel in 0..self.channels {
            match self.samples.next() {
                Some(Ok(sample)) => sum += sample,
                // a torn frame at the tail is not worth failing the whole mix over
                Some(Err(_)) | None => return (channel > 0).then_some(sum / self.channels as f32),
            }
        }
        Some(sum / self.channels as f32)
    }
}

/// A stem being read at some other rate than its own. `at` only ever moves
/// forward, which is what lets this stay a stream instead of a buffer.
struct Track {
    source: MonoReader,
    rate: f64,
    /// Source index of `window[0]`.
    position: f64,
    window: [f32; 2],
}

impl Track {
    fn open(path: &Path) -> Result<(Self, f64, u32), String> {
        let (mut source, rate, frames) = MonoReader::open(path)?;
        let window = [
            source.next_frame().unwrap_or(0.0),
            source.next_frame().unwrap_or(0.0),
        ];
        let track = Self {
            source,
            rate,
            position: 0.0,
            window,
        };
        Ok((track, rate, frames))
    }

    /// The value at `seconds`, linearly interpolated. Reads past the end return
    /// silence, so a stem that stopped early just drops out of the mix.
    fn at(&mut self, seconds: f64) -> f32 {
        let target = seconds * self.rate;
        while target - self.position >= 1.0 {
            self.window[0] = self.window[1];
            self.window[1] = self.source.next_frame().unwrap_or(0.0);
            self.position += 1.0;
        }
        let fraction = (target - self.position).max(0.0) as f32;
        self.window[0] + (self.window[1] - self.window[0]) * fraction
    }
}

/// Frames the mix needs to cover both stems, at the output rate.
fn mixed_frames(stems: &[(f64, u32)], rate: f64) -> usize {
    stems
        .iter()
        .map(|(stem_rate, frames)| (f64::from(*frames) / stem_rate.max(1.0) * rate).ceil() as usize)
        .max()
        .unwrap_or(0)
}

fn each_frame(
    system_path: &Path,
    mic_path: Option<&Path>,
    frames: usize,
    rate: f64,
    mut visit: impl FnMut(f32) -> Result<(), String>,
) -> Result<(), String> {
    let (mut system, _, _) = Track::open(system_path)?;
    let mut microphone = match mic_path {
        Some(path) => Some(Track::open(path)?.0),
        None => None,
    };

    for frame in 0..frames {
        let at = frame as f64 / rate;
        let mut sample = system.at(at);
        if let Some(microphone) = microphone.as_mut() {
            sample += microphone.at(at);
        }
        visit(sample)?;
    }
    Ok(())
}

/// Sums the stems into `out` at the system stem's rate, resampling the mic to
/// match. Returns the peak of the mix before any scaling.
///
/// The two tracks run off independent clocks, so they are aligned at the start
/// and drift apart by whatever those clocks disagree on. Fine to listen back
/// to, not sample-accurate.
pub fn mix(system_path: &Path, mic_path: Option<&Path>, out: &Path) -> Result<f32, String> {
    let (_, system_rate, system_frames) = MonoReader::open(system_path)?;
    let rate = system_rate.max(1.0);

    let mut stems = vec![(system_rate, system_frames)];
    if let Some(path) = mic_path {
        let (_, mic_rate, mic_frames) = MonoReader::open(path)?;
        stems.push((mic_rate, mic_frames));
    }
    let frames = mixed_frames(&stems, rate);

    let mut peak = 0f32;
    each_frame(system_path, mic_path, frames, rate, |sample| {
        if sample.abs() > peak {
            peak = sample.abs();
        }
        Ok(())
    })?;

    // summing two loud tracks clips, so scale the mix back by exactly what it
    // overshot instead of clamping every sample
    let gain = if peak > 1.0 { 1.0 / peak } else { 1.0 };

    let spec = WavSpec {
        channels: 1,
        sample_rate: rate.round() as u32,
        bits_per_sample: 16,
        sample_format: SampleFormat::Int,
    };
    let mut writer = WavWriter::create(out, spec).map_err(|error| error.to_string())?;
    each_frame(system_path, mic_path, frames, rate, |sample| {
        let scaled = (sample * gain).clamp(-1.0, 1.0) * f32::from(i16::MAX);
        writer
            .write_sample(scaled.round() as i16)
            .map_err(|error| error.to_string())
    })?;
    writer.finalize().map_err(|error| error.to_string())?;

    Ok(peak)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{env, f64::consts::TAU, fs, path::PathBuf};

    fn sine(name: &str, rate: u32, seconds: f64, hz: f64, amplitude: f32) -> PathBuf {
        let dir = env::temp_dir().join("mrec-mix-tests");
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join(format!("{name}.wav"));
        let spec = WavSpec {
            channels: 1,
            sample_rate: rate,
            bits_per_sample: 16,
            sample_format: SampleFormat::Int,
        };
        let mut writer = WavWriter::create(&path, spec).unwrap();
        let frames = (f64::from(rate) * seconds).round() as usize;
        for frame in 0..frames {
            let value = (TAU * hz * frame as f64 / f64::from(rate)).sin() as f32 * amplitude;
            writer
                .write_sample((value * f32::from(i16::MAX)).round() as i16)
                .unwrap();
        }
        writer.finalize().unwrap();
        path
    }

    /// `(sample rate, channels, samples)` of a finished mix.
    fn read(path: &Path) -> (u32, u16, Vec<f32>) {
        let mut reader = WavReader::open(path).unwrap();
        let spec = reader.spec();
        let scale = 1.0 / f32::from(i16::MAX);
        let samples = reader
            .samples::<i16>()
            .map(|sample| f32::from(sample.unwrap()) * scale)
            .collect();
        (spec.sample_rate, spec.channels, samples)
    }

    fn loudest(samples: &[f32]) -> f32 {
        samples.iter().fold(0.0f32, |peak, s| peak.max(s.abs()))
    }

    #[test]
    fn resamples_the_mic_to_the_system_rate() {
        let system = sine("sys-rate", 48_000, 1.0, 440.0, 0.5);
        let mic = sine("mic-rate", 16_000, 1.0, 440.0, 0.5);
        let out = env::temp_dir().join("mrec-mix-tests/out-rate.wav");

        let peak = mix(&system, Some(&mic), &out).unwrap();
        let (rate, channels, samples) = read(&out);

        assert_eq!(rate, 48_000);
        assert_eq!(channels, 1);
        assert!(
            (samples.len() as i64 - 48_000).abs() <= 2,
            "{}",
            samples.len()
        );
        // two half-scale sines in phase land at full scale
        assert!((0.9..1.05).contains(&peak), "peak {peak}");
        assert!(loudest(&samples) <= 1.0001);
    }

    #[test]
    fn scales_back_instead_of_clipping() {
        let system = sine("sys-loud", 48_000, 0.5, 300.0, 0.9);
        let mic = sine("mic-loud", 48_000, 0.5, 300.0, 0.9);
        let out = env::temp_dir().join("mrec-mix-tests/out-loud.wav");

        let peak = mix(&system, Some(&mic), &out).unwrap();
        let max = loudest(&read(&out).2);

        assert!(peak > 1.7, "sum should overshoot, got {peak}");
        assert!(
            (0.99..=1.0001).contains(&max),
            "should normalise, got {max}"
        );
    }

    #[test]
    fn covers_the_longer_stem() {
        let system = sine("sys-short", 48_000, 0.2, 440.0, 0.4);
        let mic = sine("mic-long", 44_100, 1.0, 440.0, 0.4);
        let out = env::temp_dir().join("mrec-mix-tests/out-long.wav");

        mix(&system, Some(&mic), &out).unwrap();
        let (_, _, samples) = read(&out);

        assert!(
            (samples.len() as i64 - 48_000).abs() <= 2,
            "{}",
            samples.len()
        );
        // the mic keeps going after the system stem ends
        assert!(loudest(&samples[samples.len() - 1000..]) > 0.05);
    }

    #[test]
    fn passes_a_lone_system_stem_through() {
        let system = sine("sys-only", 48_000, 0.3, 1000.0, 0.6);
        let out = env::temp_dir().join("mrec-mix-tests/out-only.wav");

        let peak = mix(&system, None, &out).unwrap();
        let (rate, _, samples) = read(&out);

        assert_eq!(rate, 48_000);
        assert!(
            (samples.len() as i64 - 14_400).abs() <= 2,
            "{}",
            samples.len()
        );
        assert!((peak - 0.6).abs() < 0.01, "peak {peak}");
    }

    #[test]
    fn resampling_stays_continuous() {
        let system = sine("sys-seam", 48_000, 2.0, 220.0, 0.0);
        let mic = sine("mic-seam", 22_050, 2.0, 220.0, 0.3);
        let out = env::temp_dir().join("mrec-mix-tests/out-seam.wav");

        mix(&system, Some(&mic), &out).unwrap();
        let (_, _, samples) = read(&out);

        let jumps = samples
            .windows(2)
            .filter(|pair| (pair[1] - pair[0]).abs() > 0.05)
            .count();
        assert_eq!(jumps, 0, "{jumps} discontinuities in the resampled mic");
    }
}
