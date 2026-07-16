// Prevents an extra console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::collections::HashMap;
use std::path::PathBuf;
use std::process;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use hound::{SampleFormat, WavSpec, WavWriter};
use meeting_record::{
    capture, meetings, permissions, AudioBuffer, CaptureOptions, CaptureSession, CaptureTarget,
    Meeting, MeetingEvent, Permission, PermissionStatus, Watcher,
};
use rtrb::RingBuffer;
use serde::Serialize;
use tauri::async_runtime;
use tauri::{AppHandle, Builder, Emitter, Manager, RunEvent, State};

/// ~1.4s of mono 48k. Only has to cover the gap between writer-thread polls.
const RING_SAMPLES: usize = 1 << 16;
const POLL: Duration = Duration::from_millis(10);
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
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RecordingStopped {
    path: String,
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
    capture: CaptureSession,
    stop: Arc<AtomicBool>,
    writer: JoinHandle<Summary>,
    path: PathBuf,
}

#[derive(Default)]
struct Summary {
    seconds: f64,
    peak: f32,
    dropped: u64,
}

fn id_of(meeting: &Meeting) -> String {
    format!("{}:{}", meeting.platform.as_str(), meeting.pid)
}

fn view(meeting: &Meeting) -> MeetingView {
    MeetingView {
        id: id_of(meeting),
        // stable identifier, not a label. the window maps it for display
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
    if name == "accessibility" {
        Permission::Accessibility
    } else {
        Permission::SystemAudio
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

fn wav_path(app: &AppHandle) -> PathBuf {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|since| since.as_millis())
        .unwrap_or(0);
    let downloads = app
        .path()
        .download_dir()
        .unwrap_or_else(|_| PathBuf::from("."));
    downloads.join(format!("meeting-record-tauri-{millis}.wav"))
}

#[tauri::command]
async fn start_recording(
    app: AppHandle,
    target_id: Option<String>,
) -> Result<RecordingStarted, String> {
    // first call can block ~6s while the permission grant is undetermined, so keep
    // it off the UI thread
    async_runtime::spawn_blocking(move || start_blocking(&app, target_id))
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

fn start_blocking(app: &AppHandle, target_id: Option<String>) -> Result<RecordingStarted, String> {
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

    let (mut producer, mut consumer) = RingBuffer::<f32>::new(RING_SAMPLES);
    let dropped = Arc::new(AtomicU64::new(0));

    let capture = {
        let dropped = Arc::clone(&dropped);
        // realtime audio thread. copy into the ring and return: no allocation, no
        // locks, no emit. write_chunk_uninit fails rather than blocking when the
        // writer thread has fallen behind.
        let handler =
            move |buffer: AudioBuffer<'_>| match producer.write_chunk_uninit(buffer.frames.len()) {
                Ok(chunk) => {
                    chunk.fill_from_iter(buffer.frames.iter().copied());
                }
                Err(_) => {
                    dropped.fetch_add(buffer.frames.len() as u64, Ordering::Relaxed);
                }
            };

        capture::start(
            CaptureTarget::Process { pid },
            CaptureOptions::default(),
            handler,
        )
    }
    .map_err(|error| error.to_string())?;

    let sample_rate = capture.sample_rate;
    let channels = capture.channels.max(1);
    let path = wav_path(app);

    let spec = WavSpec {
        channels: channels as u16,
        sample_rate: sample_rate.round() as u32,
        bits_per_sample: 16,
        sample_format: SampleFormat::Int,
    };
    // on the error path `capture` drops here, which stops the tap
    let mut wav = WavWriter::create(&path, spec).map_err(|error| error.to_string())?;

    let stop = Arc::new(AtomicBool::new(false));
    let writer = {
        let app = app.clone();
        let stop = Arc::clone(&stop);
        let dropped = Arc::clone(&dropped);

        thread::spawn(move || {
            let started = Instant::now();
            let mut last_emit = Instant::now();
            let mut window_peak = 0f32;
            let mut peak = 0f32;

            loop {
                // read the flag first, so the drain below is the final one
                let finishing = stop.load(Ordering::Acquire);

                let slots = consumer.slots();
                if slots > 0 {
                    if let Ok(chunk) = consumer.read_chunk(slots) {
                        let (head, tail) = chunk.as_slices();
                        for &sample in head.iter().chain(tail) {
                            let magnitude = sample.abs();
                            if magnitude > window_peak {
                                window_peak = magnitude;
                            }
                            // clamp before scaling: a tap mixing several processes
                            // can exceed 1.0 and the wrap sounds like loud clicks
                            let scaled = sample.clamp(-1.0, 1.0) * 32767.0;
                            let _ = wav.write_sample(scaled.round() as i16);
                        }
                        chunk.commit_all();
                    }
                }
                if window_peak > peak {
                    peak = window_peak;
                }

                if last_emit.elapsed() >= METER_INTERVAL {
                    // aggregates only, at 10Hz
                    let _ = app.emit(
                        "recording:meter",
                        Meter {
                            seconds: started.elapsed().as_secs_f64(),
                            level: window_peak,
                        },
                    );
                    window_peak = 0.0;
                    last_emit = Instant::now();
                }

                if finishing {
                    break;
                }
                thread::sleep(POLL);
            }

            let frames = wav.duration();
            let _ = wav.finalize();
            Summary {
                seconds: f64::from(frames) / sample_rate.max(1.0),
                peak,
                dropped: dropped.load(Ordering::Relaxed),
            }
        })
    };

    session.inner.lock().unwrap().recording = Some(Recording {
        capture,
        stop,
        writer,
        path: path.clone(),
    });

    Ok(RecordingStarted {
        path: path.to_string_lossy().into_owned(),
        sample_rate,
        channels,
    })
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
        stop,
        writer,
        path,
    } = recording;

    drop(capture); // stop the tap before touching the file
    stop.store(true, Ordering::Release);
    let summary = writer.join().unwrap_or_default();

    RecordingStopped {
        path: path.to_string_lossy().into_owned(),
        seconds: summary.seconds,
        peak: summary.peak,
        dropped: summary.dropped,
    }
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
            stop_recording
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
