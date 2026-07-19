// Prevents an extra console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod mix;

use std::collections::HashMap;
use std::path::PathBuf;
use std::process::{self, Command};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use hound::{SampleFormat, WavSpec, WavWriter};
use meeting_record::{
    capture, meetings, permissions, AudioTrack, CaptureOptions, CaptureSession, CaptureState,
    CaptureTarget, Meeting, MeetingEvent, MicrophoneSource, Permission, PermissionStatus, Watcher,
};
use serde::Serialize;
use tauri::async_runtime;
use tauri::{AppHandle, Builder, Emitter, Manager, RunEvent, State};

const METER_INTERVAL: Duration = Duration::from_millis(100);

#[derive(Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct MeetingView {
    id: String,
    platform: String,
    app_name: String,
    title: String,
    url: String,
    is_using_mic: bool,
    is_playing_audio: bool,
    confidence: i32,
    should_record: bool,
}

#[derive(Serialize, Clone)]
struct MeetingEventPayload {
    kind: &'static str,
    meeting: MeetingView,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RecordingStarted {
    path: String,
    sample_rate: f64,
    channels: u32,
    microphone: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RecordingStopped {
    path: String,
    system_path: String,
    mic_path: Option<String>,
    seconds: f64,
    peak: f32,
    dropped: u64,
}

#[derive(Serialize, Clone)]
struct Meter {
    seconds: f64,
    level: f32,
}

#[derive(Default)]
struct Session {
    inner: Mutex<Inner>,
}

#[derive(Default)]
struct Inner {
    watcher: Option<Watcher>,
    /// Keyed by MeetingView::id so recording can resolve the selected app pid.
    meetings: HashMap<String, u32>,
    recording: Option<Recording>,
}

struct Recording {
    capture: Arc<CaptureSession>,
    system: JoinHandle<Summary>,
    microphone: Option<JoinHandle<Summary>>,
    meter: JoinHandle<()>,
    system_path: PathBuf,
    mic_path: Option<PathBuf>,
    mixed_path: PathBuf,
}

#[derive(Default)]
struct Summary {
    seconds: f64,
    peak: f32,
    dropped: u64,
}

/// Loudest sample since the meter last read it, shared by both writer threads.
/// f32 bits compare like the floats do for non-negative values, so `fetch_max`
/// on the bit pattern is a max on magnitudes.
#[derive(Default)]
struct WindowPeak(AtomicU32);

impl WindowPeak {
    fn offer(&self, magnitude: f32) {
        self.0.fetch_max(magnitude.to_bits(), Ordering::Relaxed);
    }

    fn take(&self) -> f32 {
        f32::from_bits(self.0.swap(0, Ordering::Relaxed))
    }
}

fn id_of(meeting: &Meeting) -> String {
    format!("{}:{}", meeting.platform.as_str(), meeting.pid)
}

fn view(meeting: &Meeting) -> MeetingView {
    MeetingView {
        id: id_of(meeting),
        platform: meeting.platform.as_str().to_owned(),
        app_name: meeting.app_name.clone(),
        title: meeting.title.clone(),
        url: meeting.url.clone(),
        is_using_mic: meeting.is_using_mic,
        is_playing_audio: meeting.is_playing_audio,
        confidence: meeting.confidence,
        should_record: meeting.should_record,
    }
}

fn permission_of(name: &str) -> Permission {
    match name {
        "accessibility" => Permission::Accessibility,
        "microphone" => Permission::Microphone,
        _ => Permission::SystemAudio,
    }
}

fn label(status: PermissionStatus) -> &'static str {
    match status {
        PermissionStatus::Granted => "granted",
        PermissionStatus::Denied => "denied",
        PermissionStatus::NotRequired => "not-required",
        PermissionStatus::Unknown => "unknown",
    }
}

#[tauri::command]
fn permission_status(name: String) -> &'static str {
    label(permissions::status(permission_of(&name)))
}

#[tauri::command]
async fn request_permission(name: String) -> &'static str {
    let permission = permission_of(&name);
    let current = permissions::status(permission);
    if matches!(
        current,
        PermissionStatus::Granted | PermissionStatus::NotRequired
    ) {
        return label(current);
    }

    async_runtime::spawn_blocking(move || {
        if permission == Permission::Accessibility {
            // opens System Settings, and the grant only lands after a relaunch, so
            // there is nothing here to wait for
            if permissions::request(permission).is_err() {
                return "unknown";
            }
            return label(permissions::status(permission));
        }

        if permissions::request(permission).is_err() {
            return "unknown";
        }
        // the dialog is modal to the user, not to us
        let deadline = Instant::now() + Duration::from_secs(120);
        while Instant::now() < deadline {
            thread::sleep(Duration::from_millis(500));
            let status = permissions::status(permission);
            if matches!(status, PermissionStatus::Granted | PermissionStatus::Denied) {
                return label(status);
            }
        }
        label(permissions::status(permission))
    })
    .await
    .unwrap_or("unknown")
}

#[tauri::command]
fn scan(session: State<'_, Session>) -> Vec<MeetingView> {
    let found = meetings::scan();
    let views = found.iter().map(view).collect();
    let mut inner = session.inner.lock().unwrap();
    inner.meetings = found
        .iter()
        .map(|meeting| (id_of(meeting), meeting.pid))
        .collect();
    views
}

fn watch(app: &AppHandle) {
    let handle = app.clone();

    // the library runs this on its own serial queue, not a realtime thread, so
    // emitting straight to the webview is fine here
    let watcher = meetings::watch(move |event, meeting| {
        let kind = match event {
            MeetingEvent::Started => "started",
            MeetingEvent::Updated => "updated",
            MeetingEvent::Ended => "ended",
        };

        {
            let session = handle.state::<Session>();
            let mut inner = session.inner.lock().unwrap();
            let id = id_of(meeting);
            if event == MeetingEvent::Ended {
                inner.meetings.remove(&id);
            } else {
                inner.meetings.insert(id, meeting.pid);
            }
        }

        let _ = handle.emit(
            "meeting:event",
            MeetingEventPayload {
                kind,
                meeting: view(meeting),
            },
        );
    });

    match watcher {
        Ok(guard) => app.state::<Session>().inner.lock().unwrap().watcher = Some(guard),
        Err(error) => eprintln!("could not start watching: {error}"),
    }
}

/// `(mixed, system stem, mic stem)` for one recording.
fn wav_paths(app: &AppHandle) -> (PathBuf, PathBuf, PathBuf) {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|since| since.as_millis())
        .unwrap_or(0);
    let downloads = app
        .path()
        .download_dir()
        .unwrap_or_else(|_| PathBuf::from("."));
    let name = format!("meeting-record-tauri-{millis}");
    (
        downloads.join(format!("{name}.wav")),
        downloads.join(format!("{name}-system.wav")),
        downloads.join(format!("{name}-mic.wav")),
    )
}

#[tauri::command]
async fn start_recording(
    app: AppHandle,
    target_id: Option<String>,
    microphone: bool,
) -> Result<RecordingStarted, String> {
    // first call can block ~6s while the permission grant is undetermined, so keep
    // it off the UI thread
    async_runtime::spawn_blocking(move || start_blocking(&app, target_id, microphone))
        .await
        .map_err(|error| error.to_string())?
}

/// The system mix is silent whenever the default output device is muted or isn't
/// the one actually playing, so aim at a process that is definitely producing
/// audio. The library resolves helper pids from this one itself.
fn playing_pid() -> Result<u32, String> {
    meetings::audio_processes()
        .into_iter()
        .find(|entry| entry.is_playing_audio)
        .map(|entry| entry.pid)
        .ok_or_else(|| "nothing is playing audio".to_string())
}

/// Picks one track out of the session. A plain fn pointer so the writer thread
/// can re-borrow the track from the `Arc` it owns.
type TrackOf = fn(&CaptureSession) -> Option<&AudioTrack>;

/// Drains one track to its own WAV until the capture stops.
fn spawn_writer(
    capture: Arc<CaptureSession>,
    track_of: TrackOf,
    path: PathBuf,
    window: Arc<WindowPeak>,
) -> Result<JoinHandle<Summary>, String> {
    let track = track_of(&capture).ok_or("track is not available")?;
    let sample_rate = track.sample_rate();
    let spec = WavSpec {
        channels: track.channels().max(1) as u16,
        sample_rate: sample_rate.round() as u32,
        bits_per_sample: 16,
        sample_format: SampleFormat::Int,
    };
    // on the error path `capture` drops here, which stops the tap
    let mut wav = WavWriter::create(&path, spec).map_err(|error| error.to_string())?;

    Ok(thread::spawn(move || {
        let track = track_of(&capture).expect("track was resolved before the thread started");
        let mut peak = 0f32;

        while let Some(chunk) = track.recv() {
            for sample in chunk.frames {
                let magnitude = sample.abs();
                if magnitude > peak {
                    peak = magnitude;
                }
                window.offer(magnitude);
                // clamp before scaling: a tap mixing several processes
                // can exceed 1.0 and the wrap sounds like loud clicks
                let scaled = sample.clamp(-1.0, 1.0) * 32767.0;
                let _ = wav.write_sample(scaled.round() as i16);
            }
        }

        let frames = wav.duration();
        let _ = wav.finalize();
        Summary {
            seconds: f64::from(frames) / sample_rate.max(1.0),
            peak,
            dropped: track.dropped_samples() as u64,
        }
    }))
}

/// Ticks off a timer rather than off arriving audio, so the elapsed clock keeps
/// moving through silence.
fn spawn_meter(
    app: AppHandle,
    capture: Arc<CaptureSession>,
    window: Arc<WindowPeak>,
) -> JoinHandle<()> {
    thread::spawn(move || {
        let started = Instant::now();
        while capture.state() != CaptureState::Stopped {
            thread::sleep(METER_INTERVAL);
            let _ = app.emit(
                "recording:meter",
                Meter {
                    seconds: started.elapsed().as_secs_f64(),
                    level: window.take(),
                },
            );
        }
    })
}

fn start_blocking(
    app: &AppHandle,
    target_id: Option<String>,
    microphone: bool,
) -> Result<RecordingStarted, String> {
    let session = app.state::<Session>();

    let pid = {
        let inner = session.inner.lock().unwrap();
        if inner.recording.is_some() {
            return Err("already recording".into());
        }
        match &target_id {
            Some(id) => inner
                .meetings
                .get(id)
                .copied()
                .ok_or("meeting is no longer detected")?,
            None => playing_pid()?,
        }
    };

    let options = CaptureOptions {
        microphone: microphone.then_some(MicrophoneSource::Default),
        ..CaptureOptions::default()
    };
    let capture = Arc::new(
        capture::start(CaptureTarget::Process { pid }, options)
            .map_err(|error| error.to_string())?,
    );

    let sample_rate = capture.system_audio().sample_rate();
    let channels = capture.system_audio().channels().max(1);
    let (mixed_path, system_path, mic_path) = wav_paths(app);
    let mic_path = capture.microphone().map(|_| mic_path);

    let window = Arc::new(WindowPeak::default());
    let system = spawn_writer(
        Arc::clone(&capture),
        |capture| Some(capture.system_audio()),
        system_path.clone(),
        Arc::clone(&window),
    )?;
    let microphone = match &mic_path {
        Some(path) => Some(spawn_writer(
            Arc::clone(&capture),
            CaptureSession::microphone,
            path.clone(),
            Arc::clone(&window),
        )?),
        None => None,
    };
    let meter = spawn_meter(app.clone(), Arc::clone(&capture), window);

    let started = RecordingStarted {
        path: system_path.to_string_lossy().into_owned(),
        sample_rate,
        channels,
        microphone: microphone.is_some(),
    };

    session.inner.lock().unwrap().recording = Some(Recording {
        capture,
        system,
        microphone,
        meter,
        system_path,
        mic_path,
        mixed_path,
    });

    Ok(started)
}

#[tauri::command]
async fn stop_recording(app: AppHandle) -> Result<RecordingStopped, String> {
    async_runtime::spawn_blocking(move || {
        let recording = app
            .state::<Session>()
            .inner
            .lock()
            .unwrap()
            .recording
            .take()
            .ok_or_else(|| "not recording".to_string())?;
        Ok(finish(recording))
    })
    .await
    .map_err(|error| error.to_string())?
}

fn finish(recording: Recording) -> RecordingStopped {
    let Recording {
        capture,
        system,
        microphone,
        meter,
        system_path,
        mic_path,
        mixed_path,
    } = recording;

    capture.stop();
    // the stems are only complete once their writers have joined, and the mix
    // reads them straight back off disk
    let system_summary = system.join().unwrap_or_default();
    let mic_summary = microphone.map(|handle| handle.join().unwrap_or_default());
    let _ = meter.join();

    let seconds = system_summary
        .seconds
        .max(mic_summary.as_ref().map_or(0.0, |summary| summary.seconds));
    let dropped =
        system_summary.dropped + mic_summary.as_ref().map_or(0, |summary| summary.dropped);
    let stem_peak = system_summary
        .peak
        .max(mic_summary.as_ref().map_or(0.0, |summary| summary.peak));

    let (path, peak) = match mix::mix(&system_path, mic_path.as_deref(), &mixed_path) {
        Ok(peak) => (mixed_path, peak),
        // the stems survive a failed mix, so hand back the one that is definitely playable
        Err(error) => {
            eprintln!("could not mix the stems: {error}");
            (system_path.clone(), stem_peak)
        }
    };

    RecordingStopped {
        path: path.to_string_lossy().into_owned(),
        system_path: system_path.to_string_lossy().into_owned(),
        mic_path: mic_path.map(|path| path.to_string_lossy().into_owned()),
        seconds,
        peak,
        dropped,
    }
}

#[tauri::command]
fn reveal(path: String) -> Result<(), String> {
    Command::new("open")
        .args(["-R", &path])
        .spawn()
        .map(drop)
        .map_err(|error| error.to_string())
}

/// Both guards have to be released before the process goes away: leaking a
/// running capture wedges OS audio state, sometimes coreaudiod machine-wide.
fn stop_all(app: &AppHandle) {
    let (watcher, recording) = {
        let session = app.state::<Session>();
        let mut inner = session.inner.lock().unwrap();
        (inner.watcher.take(), inner.recording.take())
    };

    // outside the lock: dropping the watcher waits for in-flight callbacks, and
    // those take this same lock
    drop(watcher);
    if let Some(recording) = recording {
        finish(recording);
    }
}

fn main() {
    Builder::default()
        .manage(Session::default())
        .invoke_handler(tauri::generate_handler![
            permission_status,
            request_permission,
            scan,
            start_recording,
            stop_recording,
            reveal
        ])
        .setup(|app| {
            let handle = app.handle().clone();
            watch(&handle);
            // ctrl-c under `tauri dev` skips RunEvent::Exit entirely
            ctrlc::set_handler(move || {
                stop_all(&handle);
                process::exit(0);
            })?;
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("failed to build the app")
        .run(|app, event| {
            if matches!(event, RunEvent::ExitRequested { .. } | RunEvent::Exit) {
                stop_all(app);
            }
        });
}
