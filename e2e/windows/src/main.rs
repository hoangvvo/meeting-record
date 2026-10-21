//! Rust live-audio harness. Run through e2e/windows.ps1.

use std::{
    env,
    error::Error as StdError,
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc, Arc,
    },
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use meeting_record::{
    capture, meetings, AudioChunk, CaptureOptions, CaptureState, CaptureTarget, InterruptionReason,
    MeetingEvent, MicrophoneSource, MixedAudioOptions, Track, TrackAvailability, TrackEvent,
};

/// A timestamp further than this from the previous chunk's end is a gap.
const CONTINUITY_TOLERANCE_NS: u64 = 2_000_000;
/// A tone below this normalised power ratio is absent.
const ABSENT_TONE_RATIO: f64 = 0.05;
/// An expected tone must reach this ratio; two equal tones each measure 0.5.
const PRESENT_TONE_RATIO: f64 = 0.1;
/// Samples below this level count towards a dropout.
const SILENCE_LEVEL: f32 = 1e-4;
/// Silence longer than this inside a continuous tone is a dropout.
const MIN_DROPOUT: Duration = Duration::from_millis(1);
/// Allow occasional device dropouts.
const MAX_MICROPHONE_DROPOUTS: usize = 2;
/// Allow startup silence while sources attach and fill their buffers.
const STARTUP: Duration = Duration::from_millis(100);
/// System audio already in flight when the exited app is reported.
const MAX_AFTER_EXIT: Duration = Duration::from_millis(50);

#[derive(Default)]
struct TrackMetrics {
    frames: u64,
    samples: usize,
    seconds: f64,
    peak: f32,
    dropped_samples: usize,
    malformed_chunks: usize,
    timestamp_regressions: usize,
    gaps_ns: Vec<u64>,
    first_host_time_ns: Option<u64>,
    last_host_time_ns: Option<u64>,
    next_host_time_ns: Option<u64>,
    sample_rate: f64,
    mono: Vec<f32>,
}

impl TrackMetrics {
    fn add(&mut self, chunk: AudioChunk) {
        let expected_samples = chunk.frame_count as usize * chunk.channels as usize;
        if chunk.frames.len() != expected_samples || chunk.channels == 0 || chunk.sample_rate <= 0.0
        {
            self.malformed_chunks += 1;
            return;
        }
        if let Some(previous) = self.last_host_time_ns {
            if chunk.host_time_ns < previous {
                self.timestamp_regressions += 1;
            }
        } else {
            self.first_host_time_ns = Some(chunk.host_time_ns);
        }
        if let Some(expected) = self.next_host_time_ns {
            if chunk.host_time_ns.abs_diff(expected) > CONTINUITY_TOLERANCE_NS {
                self.gaps_ns
                    .push(chunk.host_time_ns.saturating_sub(expected));
            }
        }
        let duration_ns = f64::from(chunk.frame_count) * 1e9 / chunk.sample_rate;
        self.next_host_time_ns = Some(chunk.host_time_ns + duration_ns.round() as u64);
        self.last_host_time_ns = Some(chunk.host_time_ns);
        self.frames += u64::from(chunk.frame_count);
        self.samples += chunk.frames.len();
        self.seconds += f64::from(chunk.frame_count) / chunk.sample_rate;
        self.dropped_samples += chunk.dropped_samples;
        self.sample_rate = chunk.sample_rate;
        for frame in chunk.frames.chunks_exact(chunk.channels as usize) {
            for sample in frame {
                self.peak = self.peak.max(sample.abs());
            }
            self.mono
                .push(frame.iter().sum::<f32>() / chunk.channels as f32);
        }
    }

    fn verify(
        &self,
        name: &str,
        expected_duration: Duration,
        expectations: &Expectations,
    ) -> Result<(), String> {
        let minimum_seconds = expected_duration.as_secs_f64() * 0.7;
        if self.frames == 0 || self.seconds < minimum_seconds {
            return Err(format!(
                "{name}: captured only {:.2}s ({}) frames; expected at least {:.2}s",
                self.seconds, self.frames, minimum_seconds
            ));
        }
        if self.peak < expectations.minimum_peak {
            return Err(format!(
                "{name}: peak {:.6} is below required {:.6}",
                self.peak, expectations.minimum_peak
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
        if !expectations.allow_drops && self.dropped_samples != 0 {
            return Err(format!(
                "{name}: {} samples were dropped",
                self.dropped_samples
            ));
        }
        verify_gaps(name, &self.gaps_ns, expectations.pause)
    }

    /// An interruption leaves one timestamp gap, and moving to another device
    /// none: audio is neither backfilled nor overlapped.
    fn verify_recovered(&self, name: &str, interrupted: bool) -> Result<(), String> {
        if self.malformed_chunks != 0 || self.timestamp_regressions != 0 {
            return Err(format!(
                "{name}: malformed chunks or timestamp regressions around recovery"
            ));
        }
        if self.gaps_ns.len() != usize::from(interrupted) {
            return Err(format!(
                "{name}: timestamp gaps {:?} do not match the removal",
                self.gaps_ns
                    .iter()
                    .map(|gap| Duration::from_nanos(*gap))
                    .collect::<Vec<_>>()
            ));
        }
        Ok(())
    }

    fn verify_tones(&self, name: &str, expected: &[f64], rejected: &[f64]) -> Result<(), String> {
        verify_tones(name, &self.mono, self.sample_rate, expected, rejected)
    }

    fn verify_continuous(&self, name: &str, allowed: usize) -> Result<(), String> {
        verify_continuous(name, &self.mono, self.sample_rate, allowed)
    }

    fn report(&self, name: &str) {
        println!(
            "{name}: {:.2}s, {} frames, {} samples, peak={:.6}, dropped={}, gaps={:?}, \
             timestamps={}..{}",
            self.seconds,
            self.frames,
            self.samples,
            self.peak,
            self.dropped_samples,
            self.gaps_ns
                .iter()
                .map(|gap| Duration::from_nanos(*gap))
                .collect::<Vec<_>>(),
            self.first_host_time_ns.unwrap_or(0),
            self.last_host_time_ns.unwrap_or(0),
        );
    }
}

struct Expectations {
    minimum_peak: f32,
    allow_drops: bool,
    pause: Option<Duration>,
    expected_frequencies: Vec<f64>,
    rejected_frequencies: Vec<f64>,
    microphone_frequencies: Vec<f64>,
}

/// Every gap must come from the pause: exactly one, close to its length.
fn verify_gaps(name: &str, gaps_ns: &[u64], pause: Option<Duration>) -> Result<(), String> {
    match (pause, gaps_ns) {
        (None, []) => Ok(()),
        (Some(pause), [gap]) if Duration::from_nanos(*gap) >= pause.mul_f64(0.5) => Ok(()),
        _ => Err(format!(
            "{name}: timestamp gaps {:?} do not match pause {pause:?}",
            gaps_ns
                .iter()
                .map(|gap| Duration::from_nanos(*gap))
                .collect::<Vec<_>>()
        )),
    }
}

/// Normalised single-bin DFT power per 100 ms block, averaged over blocks with
/// signal: about 1 for a pure tone at `frequency`, 0 for unrelated audio.
fn tone_ratio(samples: &[f32], sample_rate: f64, frequency: f64) -> f64 {
    let block = (sample_rate / 10.0) as usize;
    let coefficient = 2.0 * (2.0 * std::f64::consts::PI * frequency / sample_rate).cos();
    let ratios: Vec<f64> = samples
        .chunks_exact(block.max(1))
        .filter_map(|block| {
            let (mut previous, mut before_previous, mut energy) = (0.0, 0.0, 0.0);
            for sample in block {
                let sample = f64::from(*sample);
                let current = sample + coefficient * previous - before_previous;
                before_previous = previous;
                previous = current;
                energy += sample * sample;
            }
            let power = previous * previous + before_previous * before_previous
                - coefficient * previous * before_previous;
            (energy > 1e-9).then(|| 2.0 * power / (block.len() as f64 * energy))
        })
        .collect();
    if ratios.is_empty() {
        0.0
    } else {
        ratios.iter().sum::<f64>() / ratios.len() as f64
    }
}

/// Silent runs between the first and last audible samples. Capture may start or
/// stop between a source's packets.
fn dropouts(samples: &[f32], sample_rate: f64) -> Vec<Duration> {
    let audible = |sample: &f32| sample.abs() >= SILENCE_LEVEL;
    let start = samples.iter().position(audible);
    let end = samples.iter().rposition(audible);
    let (Some(start), Some(end)) = (start, end) else {
        return Vec::new();
    };
    samples[start..=end]
        .split(audible)
        .map(|run| Duration::from_secs_f64(run.len() as f64 / sample_rate))
        .filter(|run| *run > MIN_DROPOUT)
        .collect()
}

fn verify_tones(
    name: &str,
    samples: &[f32],
    sample_rate: f64,
    expected: &[f64],
    rejected: &[f64],
) -> Result<(), String> {
    for frequency in expected {
        let ratio = tone_ratio(samples, sample_rate, *frequency);
        println!("{name}: {frequency} Hz ratio {ratio:.3}");
        if ratio < PRESENT_TONE_RATIO {
            return Err(format!(
                "{name}: expected {frequency} Hz, measured ratio {ratio:.3}"
            ));
        }
    }
    for frequency in rejected {
        let ratio = tone_ratio(samples, sample_rate, *frequency);
        println!("{name}: {frequency} Hz ratio {ratio:.3} (rejected)");
        if ratio > ABSENT_TONE_RATIO {
            return Err(format!(
                "{name}: captured {frequency} Hz from outside the target (ratio {ratio:.3})"
            ));
        }
    }
    Ok(())
}

/// A continuous tone must not fall silent after startup more than `allowed`
/// times.
fn verify_continuous(
    name: &str,
    samples: &[f32],
    sample_rate: f64,
    allowed: usize,
) -> Result<(), String> {
    let startup = (STARTUP.as_secs_f64() * sample_rate) as usize;
    let dropouts = dropouts(samples.get(startup..).unwrap_or_default(), sample_rate);
    if dropouts.len() > allowed {
        return Err(format!(
            "{name}: continuous tone has dropouts {dropouts:?}; at most {allowed} allowed"
        ));
    }
    Ok(())
}

#[derive(Default)]
struct MixMetrics {
    bytes: usize,
    peak_level: f32,
    samples: Vec<f32>,
}

impl MixMetrics {
    fn verify(
        &self,
        sample_rate: u32,
        captured_seconds: f64,
        expectations: &Expectations,
    ) -> Result<(), String> {
        let seconds = self.samples.len() as f64 / f64::from(sample_rate);
        println!(
            "mixed: {seconds:.2}s at {sample_rate} Hz, {} bytes, peak level={:.6}",
            self.bytes, self.peak_level
        );
        if !self.bytes.is_multiple_of(2) {
            return Err(format!(
                "mixed: {} bytes is not whole s16le samples",
                self.bytes
            ));
        }
        if (seconds - captured_seconds).abs() > 0.25 {
            return Err(format!(
                "mixed: {seconds:.2}s does not match {captured_seconds:.2}s of system audio"
            ));
        }
        if self.peak_level <= 0.0 {
            return Err("mixed: level never rose above zero".to_owned());
        }
        // Every source's tones reach the mix, unless they are above its Nyquist
        // rate. A microphone tone the system track also carries may cancel.
        let microphone = expectations
            .microphone_frequencies
            .iter()
            .filter(|frequency| expectations.rejected_frequencies.contains(frequency));
        let audible: Vec<f64> = expectations
            .expected_frequencies
            .iter()
            .chain(microphone)
            .copied()
            .filter(|frequency| *frequency < f64::from(sample_rate) / 2.0)
            .collect();
        verify_tones(
            "mixed",
            &self.samples,
            f64::from(sample_rate),
            &audible,
            &[],
        )?;
        // The system tones keep the mix audible through any microphone dropout.
        if expectations.expected_frequencies.is_empty() {
            Ok(())
        } else {
            verify_continuous("mixed", &self.samples, f64::from(sample_rate), 0)
        }
    }
}

fn microphone_removed(event: &TrackEvent) -> bool {
    matches!(
        event,
        TrackEvent::Interrupted {
            track: Track::Microphone,
            reason: InterruptionReason::DeviceRemoved,
            ..
        }
    )
}

fn microphone_resumed(event: &TrackEvent) -> bool {
    matches!(
        event,
        TrackEvent::Resumed {
            track: Track::Microphone
        }
    )
}

fn microphone_changed(event: &TrackEvent) -> bool {
    matches!(
        event,
        TrackEvent::DeviceChanged {
            track: Track::Microphone,
            ..
        }
    )
}

/// Track events must be exactly those the scenario causes. A removed microphone
/// is interrupted until it returns, or moves to another device when one is
/// available.
fn verify_track_events(
    events: &[TrackEvent],
    expect_device_change: bool,
    expect_recovery: bool,
) -> Result<(), String> {
    let unexpected: Vec<_> = events
        .iter()
        .filter(|event| {
            !((expect_device_change || expect_recovery) && microphone_changed(event))
                && !(expect_recovery && (microphone_removed(event) || microphone_resumed(event)))
        })
        .collect();
    if !unexpected.is_empty() {
        return Err(format!("unexpected track events: {unexpected:?}"));
    }
    if expect_device_change && !events.iter().any(microphone_changed) {
        return Err("expected a microphone device change".into());
    }
    let recovered = match events.iter().position(microphone_removed) {
        Some(lost) => events[lost..].iter().any(microphone_resumed),
        None => events.iter().any(microphone_changed),
    };
    if expect_recovery && !recovered {
        return Err("expected the microphone to resume or move to another device".into());
    }
    Ok(())
}

fn enabled(name: &str) -> bool {
    env::var(name).is_ok_and(|value| matches!(value.as_str(), "1" | "true" | "yes"))
}

fn frequencies(name: &str) -> Result<Vec<f64>, Box<dyn StdError>> {
    let Ok(value) = env::var(name) else {
        return Ok(Vec::new());
    };
    value
        .split(',')
        .filter(|item| !item.trim().is_empty())
        .map(|item| Ok(item.trim().parse::<f64>()?))
        .collect()
}

struct ExcludedTone {
    child: Child,
    ready_file: PathBuf,
}

impl ExcludedTone {
    fn start(
        frequency: Option<u32>,
        duration: Duration,
    ) -> Result<Option<Self>, Box<dyn StdError>> {
        let Some(frequency) = frequency else {
            return Ok(None);
        };
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        let ready_file = env::temp_dir().join(format!(
            "meeting-record-excluded-{}-{nonce}",
            std::process::id()
        ));
        let mut command = Command::new(
            env::var_os("MREC_TONE_POWERSHELL").unwrap_or_else(|| "powershell.exe".into()),
        );
        command
            .args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-File"])
            .arg(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../windows-tone.ps1"))
            .args([
                "-Frequency",
                &frequency.to_string(),
                "-DurationSeconds",
                &(duration.as_secs() + 30).to_string(),
                "-ReadyFile",
            ])
            .arg(&ready_file);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x08000000);
        }
        let mut tone = Self {
            child: command.spawn()?,
            ready_file,
        };
        let deadline = Instant::now() + Duration::from_secs(20);
        while !tone.ready_file.exists() {
            tone.assert_alive()?;
            if Instant::now() >= deadline {
                return Err("timed out starting the excluded tone".into());
            }
            thread::sleep(Duration::from_millis(50));
        }
        tone.assert_alive()?;
        Ok(Some(tone))
    }

    fn assert_alive(&mut self) -> Result<(), Box<dyn StdError>> {
        if self.child.try_wait()?.is_some() {
            return Err("the excluded tone exited during capture".into());
        }
        Ok(())
    }
}

impl Drop for ExcludedTone {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.ready_file);
        let _ = std::fs::remove_file(
            env::temp_dir().join(format!("meeting-record-tone-{}.wav", self.child.id())),
        );
    }
}

/// The app of a meeting that both a scan and the watcher report as recordable.
fn find_meeting(platform: &str, pid: Option<u32>) -> Result<CaptureTarget, Box<dyn StdError>> {
    let found = meetings::scan()?;
    for meeting in &found {
        println!("scan: {meeting:?}");
    }
    let meeting = found
        .into_iter()
        .find(|meeting| {
            meeting.platform.as_str() == platform
                && meeting.should_record
                && pid.is_none_or(|pid| meeting.pid == pid)
        })
        .ok_or_else(|| format!("no recordable {platform} meeting was detected"))?;

    // The watcher reports a meeting once it has been seen for about 2 s.
    let (sender, receiver) = mpsc::channel();
    let watcher = meetings::watch(move |event, meeting| {
        let _ = sender.send((event, meeting.clone()));
    })?;
    let deadline = Instant::now() + Duration::from_secs(8);
    let watched = loop {
        let (event, candidate) = receiver
            .recv_timeout(deadline.saturating_duration_since(Instant::now()))
            .map_err(|_| format!("the watcher did not report the {platform} meeting"))?;
        if event == MeetingEvent::Started && candidate.pid == meeting.pid {
            break candidate;
        }
    };
    drop(watcher);
    if watched.platform != meeting.platform || !watched.should_record {
        return Err(
            format!("the watcher reported {watched:?}, but a scan found {meeting:?}").into(),
        );
    }
    println!(
        "meeting: {} pid {} {:?} {}",
        meeting.platform.as_str(),
        meeting.pid,
        meeting.title,
        meeting.url
    );
    Ok(CaptureTarget::Process { pid: meeting.pid })
}

/// Short sessions must each deliver audio, stop cleanly, and leave capture
/// idle.
fn restart_cycles(
    target: CaptureTarget,
    options: &CaptureOptions,
    cycles: u32,
) -> Result<(), Box<dyn StdError>> {
    for cycle in 1..=cycles {
        let recording = capture::start(target, options.clone())?;
        thread::sleep(Duration::from_millis(250));
        recording.stop();
        let mut frames = 0u64;
        while let Some(chunk) = recording.system_audio().recv() {
            frames += u64::from(chunk.frame_count);
        }
        if let Some(mixed) = recording.mixed_audio() {
            while mixed.recv().is_some() {}
        }
        if frames == 0 {
            return Err(format!("restart cycle {cycle}: no audio before stop").into());
        }
        if recording.state() != CaptureState::Stopped || capture::running() {
            return Err(format!("restart cycle {cycle}: capture still running after stop").into());
        }
        drop(recording);
    }
    if cycles > 0 {
        println!("restart cycles: {cycles} passed");
    }
    Ok(())
}

/// Microseconds of audio read from each output, counted by reader threads.
#[derive(Default)]
struct Progress {
    system: AtomicU64,
    microphone: AtomicU64,
    mixed: AtomicU64,
}

impl Progress {
    fn snapshot(&self) -> [Duration; 3] {
        [&self.system, &self.microphone, &self.mixed]
            .map(|micros| Duration::from_micros(micros.load(Ordering::Relaxed)))
    }
}

/// Ending the captured app stops only its system audio; the session and
/// microphone go on.
fn app_exit(target: CaptureTarget, options: CaptureOptions) -> Result<(), Box<dyn StdError>> {
    let CaptureTarget::Process { pid } = target else {
        return Err("exit: needs a process target".into());
    };
    let mix_sample_rate = options.mix.map(|mix| mix.sample_rate);
    println!("starting {target:?} capture, then ending the app");
    let recording = Arc::new(capture::start(target, options)?);
    let progress = Arc::new(Progress::default());
    let readers = [
        {
            let (recording, progress) = (Arc::clone(&recording), Arc::clone(&progress));
            thread::spawn(move || {
                while let Some(chunk) = recording.system_audio().recv() {
                    let micros = f64::from(chunk.frame_count) * 1e6 / chunk.sample_rate;
                    progress.system.fetch_add(micros as u64, Ordering::Relaxed);
                }
            })
        },
        {
            let (recording, progress) = (Arc::clone(&recording), Arc::clone(&progress));
            thread::spawn(move || {
                let Some(track) = recording.microphone() else {
                    return;
                };
                while let Some(chunk) = track.recv() {
                    let micros = f64::from(chunk.frame_count) * 1e6 / chunk.sample_rate;
                    progress
                        .microphone
                        .fetch_add(micros as u64, Ordering::Relaxed);
                }
            })
        },
        {
            let (recording, progress) = (Arc::clone(&recording), Arc::clone(&progress));
            thread::spawn(move || {
                let (Some(mixed), Some(sample_rate)) = (recording.mixed_audio(), mix_sample_rate)
                else {
                    return;
                };
                while let Some(chunk) = mixed.recv() {
                    let micros = chunk.pcm.len() as u64 / 2 * 1_000_000 / u64::from(sample_rate);
                    progress.mixed.fetch_add(micros, Ordering::Relaxed);
                }
            })
        },
    ];

    let checked = (|| -> Result<(), Box<dyn StdError>> {
        thread::sleep(Duration::from_secs(1));
        if progress.snapshot()[0].is_zero() {
            return Err("exit: no system audio before the app ended".into());
        }
        let killed = Command::new("taskkill")
            .args(["/F", "/PID", &pid.to_string()])
            .stdout(Stdio::null())
            .status()?;
        if !killed.success() {
            return Err(format!("exit: could not end pid {pid}").into());
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        let reason = loop {
            match recording.try_recv_event() {
                Some(TrackEvent::Interrupted {
                    track: Track::SystemAudio,
                    reason,
                    ..
                }) => break reason,
                Some(event) => println!("track event: {event:?}"),
                None if Instant::now() >= deadline => {
                    return Err("exit: system audio was not interrupted after the app ended".into())
                }
                None => thread::sleep(Duration::from_millis(20)),
            }
        };
        println!("system interrupted ({reason:?})");
        if reason != InterruptionReason::AppExited {
            return Err(format!("exit: interrupted for {reason:?}").into());
        }

        let before = progress.snapshot();
        thread::sleep(Duration::from_millis(1500));
        let [system, microphone, mixed] = progress.snapshot();
        let (system, microphone, mixed) = (
            system - before[0],
            microphone - before[1],
            mixed - before[2],
        );
        println!(
            "after exit: system +{:.2}s, microphone +{:.2}s, mixed +{:.2}s",
            system.as_secs_f64(),
            microphone.as_secs_f64(),
            mixed.as_secs_f64()
        );
        if system > MAX_AFTER_EXIT {
            return Err("exit: system audio continued after the app ended".into());
        }
        let microphone_track = recording.microphone();
        if microphone_track.is_some() && microphone < Duration::from_secs(1) {
            return Err("exit: the microphone stopped with the app".into());
        }
        if microphone_track.is_some()
            && recording.mixed_audio().is_some()
            && mixed < Duration::from_secs(1)
        {
            return Err("exit: mixed audio stopped with the app".into());
        }
        if recording.state() != CaptureState::Recording
            || recording.system_audio().availability() != TrackAvailability::Interrupted
            || microphone_track.is_some_and(|track| track.is_interrupted())
        {
            return Err("exit: the session did not keep recording without the app".into());
        }
        Ok(())
    })();
    recording.stop();
    for reader in readers {
        reader.join().map_err(|_| "exit: a reader panicked")?;
    }
    checked?;
    if recording.state() != CaptureState::Stopped || capture::running() {
        return Err("exit: capture still running after stop".into());
    }
    Ok(())
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
    let target_pid = env::var("MREC_TARGET_PID")
        .ok()
        .map(|value| value.parse::<u32>())
        .transpose()?;
    let target = match (env::var("MREC_EXPECT_MEETING"), target_pid) {
        (Ok(platform), pid) if !platform.is_empty() => find_meeting(&platform, pid)?,
        (_, Some(pid)) => CaptureTarget::Process { pid },
        _ => CaptureTarget::System,
    };
    let mix_sample_rate = env::var("MREC_MIX_SAMPLE_RATE")
        .ok()
        .map(|value| value.parse::<u32>())
        .transpose()?;
    let pause = env::var("MREC_PAUSE_SECONDS")
        .ok()
        .map(|value| value.parse().map(Duration::from_secs_f64))
        .transpose()?;
    let restart_count = env::var("MREC_RESTART_CYCLES")
        .ok()
        .map_or(Ok(0), |value| value.parse())?;
    let include_microphone = enabled("MREC_MICROPHONE");
    let expect_device_change = enabled("MREC_EXPECT_DEVICE_CHANGE");
    let excluded_frequency = env::var("MREC_EXCLUDED_TONE_FREQUENCY")
        .ok()
        .map(|value| value.parse::<u32>())
        .transpose()?;
    if excluded_frequency.is_some() && !matches!(target, CaptureTarget::System) {
        return Err("the recorder exclusion check needs system capture".into());
    }
    let expect_recovery = enabled("MREC_EXPECT_MICROPHONE_RECOVERY");
    let expectations = Expectations {
        minimum_peak,
        allow_drops: enabled("MREC_ALLOW_DROPS"),
        pause,
        expected_frequencies: frequencies("MREC_EXPECT_FREQUENCIES")?,
        rejected_frequencies: frequencies("MREC_REJECT_FREQUENCIES")?,
        microphone_frequencies: frequencies("MREC_MICROPHONE_FREQUENCIES")?,
    };
    let options = CaptureOptions {
        mono: enabled("MREC_MONO"),
        microphone: include_microphone.then_some(MicrophoneSource::Auto),
        mix: mix_sample_rate.map(|sample_rate| MixedAudioOptions { sample_rate }),
        ..CaptureOptions::default()
    };

    if enabled("MREC_EXPECT_APP_EXIT") {
        app_exit(target, options)?;
        println!("PASS: live audio capture met all E2E assertions");
        return Ok(());
    }

    restart_cycles(target, &options, restart_count)?;

    println!("starting {target:?} capture for {}s", duration.as_secs());
    let recording = Arc::new(capture::start(target, options)?);
    let measuring = Arc::new(AtomicBool::new(excluded_frequency.is_none()));

    let system_reader = Arc::clone(&recording);
    let system_measuring = Arc::clone(&measuring);
    let system_worker = thread::spawn(move || {
        let mut metrics = TrackMetrics::default();
        while let Some(chunk) = system_reader.system_audio().recv() {
            if system_measuring.load(Ordering::Acquire) {
                metrics.add(chunk);
            }
        }
        metrics
    });
    let microphone_worker = include_microphone.then(|| {
        let microphone_reader = Arc::clone(&recording);
        let microphone_measuring = Arc::clone(&measuring);
        thread::spawn(move || {
            let mut metrics = TrackMetrics::default();
            let track = microphone_reader
                .microphone()
                .expect("microphone track requested but missing");
            while let Some(chunk) = track.recv() {
                if microphone_measuring.load(Ordering::Acquire) {
                    metrics.add(chunk);
                }
            }
            metrics
        })
    });
    let mix_worker = mix_sample_rate.is_some().then(|| {
        let mix_reader = Arc::clone(&recording);
        let mix_measuring = Arc::clone(&measuring);
        thread::spawn(move || {
            let mut metrics = MixMetrics::default();
            let mixed = mix_reader
                .mixed_audio()
                .expect("mixed audio requested but missing");
            while let Some(chunk) = mixed.recv() {
                if !mix_measuring.load(Ordering::Acquire) {
                    continue;
                }
                metrics.bytes += chunk.pcm.len();
                metrics.peak_level = metrics.peak_level.max(chunk.level);
                metrics.samples.extend(
                    chunk
                        .pcm
                        .as_chunks::<2>()
                        .0
                        .iter()
                        .map(|bytes| f32::from(i16::from_le_bytes(*bytes)) / 32768.0),
                );
            }
            metrics
        })
    });

    let mut excluded_tone = match ExcludedTone::start(excluded_frequency, duration) {
        Ok(tone) => tone,
        Err(error) => {
            recording.stop();
            return Err(error);
        }
    };
    measuring.store(true, Ordering::Release);
    let started = Instant::now();
    let deadline = started + duration;
    // Pause once, in the middle of the recording.
    let mut pause_window = pause.map(|pause| {
        let start = started + duration.saturating_sub(pause) / 2;
        (start, start + pause)
    });
    let mut events = Vec::new();
    while Instant::now() < deadline {
        thread::sleep(Duration::from_millis(50));
        if let Some((start, end)) = pause_window {
            let now = Instant::now();
            if now >= end {
                recording.resume();
                pause_window = None;
            } else if now >= start && recording.state() == CaptureState::Recording {
                recording.pause();
            }
        }
        while let Some(event) = recording.try_recv_event() {
            println!("track event: {event:?}");
            events.push(event);
        }
    }
    let interrupted = recording.system_audio().is_interrupted()
        || recording
            .microphone()
            .is_some_and(|track| track.is_interrupted());
    recording.stop();
    if let Some(tone) = &mut excluded_tone {
        tone.assert_alive()?;
    }
    drop(excluded_tone);
    while let Some(event) = recording.try_recv_event() {
        println!("track event: {event:?}");
        events.push(event);
    }

    let system = system_worker.join().map_err(|_| "system reader panicked")?;
    let microphone = microphone_worker
        .map(|worker| worker.join().map_err(|_| "microphone reader panicked"))
        .transpose()?;
    let mixed = mix_worker
        .map(|worker| worker.join().map_err(|_| "mixed audio reader panicked"))
        .transpose()?;

    if interrupted {
        return Err("a track was still interrupted at the end".into());
    }
    verify_track_events(&events, expect_device_change, expect_recovery)?;

    let expected_duration = duration.saturating_sub(pause.unwrap_or_default());
    system.report("system");
    system.verify("system", expected_duration, &expectations)?;
    system.verify_tones(
        "system",
        &expectations.expected_frequencies,
        &expectations.rejected_frequencies,
    )?;
    if !expectations.expected_frequencies.is_empty() {
        system.verify_continuous("system", 0)?;
    }
    if let Some(microphone) = microphone {
        microphone.report("microphone");
        if expect_recovery {
            microphone.verify_recovered("microphone", events.iter().any(microphone_removed))?;
        } else {
            microphone.verify("microphone", expected_duration, &expectations)?;
        }
        microphone.verify_tones("microphone", &expectations.microphone_frequencies, &[])?;
        if !expectations.microphone_frequencies.is_empty() {
            microphone.verify_continuous("microphone", MAX_MICROPHONE_DROPOUTS)?;
        }
    }
    if let (Some(mixed), Some(sample_rate)) = (mixed, mix_sample_rate) {
        mixed.verify(sample_rate, system.seconds, &expectations)?;
    }
    println!("PASS: live audio capture met all E2E assertions");
    Ok(())
}
