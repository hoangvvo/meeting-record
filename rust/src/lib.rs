//! System-audio capture and meeting detection for macOS and Windows.
//!
//! ```no_run
//! use meeting_record as mrec;
//!
//! # fn main() -> Result<(), mrec::Error> {
//! let watcher = mrec::watch(|event, meeting| {
//!     if event == mrec::MeetingEvent::Started && meeting.should_record {
//!         println!("recording {}", meeting.platform);
//!     }
//! })?;
//! # Ok(())
//! # }
//! ```
//!
//! Two things are enforced by the type system here rather than by documentation:
//! capture and watching are process-wide singletons, so both hand back a guard
//! that stops them on drop. That matters because leaking a running capture leaves
//! OS audio state behind.

pub mod sys;

use std::ffi::{c_void, CStr};
use std::fmt;
use std::sync::Mutex;

/* ---- errors ------------------------------------------------------------ */

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    UnsupportedOs,
    /// Not granted, or still undetermined. On macOS see [`request_audio_permission`].
    Permission(String),
    AlreadyRunning,
    NotRunning,
    NoProcesses,
    Capture(String),
    Internal(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::UnsupportedOs => write!(f, "unsupported OS version"),
            Error::Permission(m) => write!(f, "permission not granted: {m}"),
            Error::AlreadyRunning => write!(f, "already running"),
            Error::NotRunning => write!(f, "not running"),
            Error::NoProcesses => write!(f, "no processes to capture"),
            Error::Capture(m) => write!(f, "capture failed: {m}"),
            Error::Internal(m) => write!(f, "internal error: {m}"),
        }
    }
}

impl std::error::Error for Error {}

fn last_error() -> String {
    unsafe {
        let ptr = sys::mrec_last_error();
        if ptr.is_null() {
            String::new()
        } else {
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        }
    }
}

fn check(status: i32) -> Result<(), Error> {
    match status {
        sys::MREC_OK => Ok(()),
        sys::MREC_ERR_UNSUPPORTED_OS => Err(Error::UnsupportedOs),
        sys::MREC_ERR_PERMISSION => Err(Error::Permission(last_error())),
        sys::MREC_ERR_ALREADY_RUNNING => Err(Error::AlreadyRunning),
        sys::MREC_ERR_NOT_RUNNING => Err(Error::NotRunning),
        sys::MREC_ERR_NO_PROCESSES => Err(Error::NoProcesses),
        sys::MREC_ERR_TAP_FAILED
        | sys::MREC_ERR_DEVICE_FAILED
        | sys::MREC_ERR_IOPROC_FAILED => Err(Error::Capture(last_error())),
        _ => Err(Error::Internal(last_error())),
    }
}

fn c_string(bytes: &[std::os::raw::c_char]) -> String {
    unsafe { CStr::from_ptr(bytes.as_ptr()).to_string_lossy().into_owned() }
}

/* ---- permissions ------------------------------------------------------- */

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Permission {
    Unknown,
    Granted,
    Denied,
    /// Windows has no equivalent grant for loopback capture.
    NotRequired,
}

impl From<i32> for Permission {
    fn from(value: i32) -> Self {
        match value {
            1 => Permission::Granted,
            2 => Permission::Denied,
            3 => Permission::NotRequired,
            _ => Permission::Unknown,
        }
    }
}

pub fn audio_permission() -> Permission {
    unsafe { sys::mrec_audio_permission_status() }.into()
}

/// Show the one-time system-audio prompt.
///
/// Returns immediately: the dialog is modal to the user, not to the caller. Poll
/// [`audio_permission`] for the answer. Requires a GUI process — see the crate
/// README.
pub fn request_audio_permission() {
    unsafe { sys::mrec_request_audio_permission() };
}

pub fn accessibility_permission() -> Permission {
    unsafe { sys::mrec_accessibility_permission_status() }.into()
}

/// Opens System Settings. The app must be relaunched before a grant takes effect.
pub fn request_accessibility_permission() {
    unsafe { sys::mrec_request_accessibility_permission() };
}

/* ---- processes --------------------------------------------------------- */

#[derive(Debug, Clone)]
pub struct AudioProcess {
    pub pid: u32,
    pub name: String,
    pub bundle_id: String,
    pub is_playing_audio: bool,
    pub is_using_mic: bool,
}

/// Every live process doing audio IO.
pub fn audio_processes() -> Vec<AudioProcess> {
    let mut buffer = vec![
        sys::Process {
            pid: 0,
            is_running_output: 0,
            is_running_input: 0,
            bundle_id: [0; 256],
            name: [0; 256],
        };
        256
    ];
    let mut count = 0usize;
    unsafe { sys::mrec_list_audio_processes(buffer.as_mut_ptr(), buffer.len(), &mut count) };

    buffer
        .iter()
        .take(count)
        .map(|p| AudioProcess {
            pid: p.pid,
            name: c_string(&p.name),
            bundle_id: c_string(&p.bundle_id),
            is_playing_audio: p.is_running_output != 0,
            is_using_mic: p.is_running_input != 0,
        })
        .collect()
}

/* ---- meetings ---------------------------------------------------------- */

#[derive(Clone)]
pub struct Meeting {
    pub platform: String,
    /// The application the user sees; not necessarily where the audio is.
    pub pid: u32,
    pub app_name: String,
    /// Requires the accessibility permission; empty otherwise.
    pub title: String,
    /// Requires the accessibility permission; empty otherwise.
    pub url: String,
    pub is_using_mic: bool,
    pub is_playing_audio: bool,
    pub confidence: i32,
    /// Prefer this over comparing `confidence`; the weights may change.
    pub should_record: bool,
    /// Retained so [`record`] can start capture without the caller handling pids.
    raw: sys::Meeting,
}

impl fmt::Debug for Meeting {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Meeting")
            .field("platform", &self.platform)
            .field("pid", &self.pid)
            .field("app_name", &self.app_name)
            .field("title", &self.title)
            .field("url", &self.url)
            .field("is_using_mic", &self.is_using_mic)
            .field("is_playing_audio", &self.is_playing_audio)
            .field("confidence", &self.confidence)
            .field("should_record", &self.should_record)
            .finish()
    }
}

impl Meeting {
    fn from_raw(raw: &sys::Meeting) -> Self {
        let platform = unsafe {
            let ptr = sys::mrec_platform_name(raw.platform);
            if ptr.is_null() {
                "Unknown".to_string()
            } else {
                CStr::from_ptr(ptr).to_string_lossy().into_owned()
            }
        };
        Meeting {
            platform,
            pid: raw.pid,
            app_name: c_string(&raw.app_name),
            title: c_string(&raw.title),
            url: c_string(&raw.url),
            is_using_mic: raw.is_using_mic != 0,
            is_playing_audio: raw.is_playing_audio != 0,
            confidence: raw.confidence,
            should_record: raw.should_record != 0,
            raw: *raw,
        }
    }
}

/// Point-in-time scan, highest confidence first.
pub fn scan() -> Vec<Meeting> {
    let mut buffer = [unsafe { std::mem::zeroed::<sys::Meeting>() }; 16];
    let mut count = 0usize;
    unsafe { sys::mrec_scan(buffer.as_mut_ptr(), buffer.len(), &mut count) };
    buffer.iter().take(count).map(Meeting::from_raw).collect()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MeetingEvent {
    Started,
    Updated,
    Ended,
}

type MeetingHandler = Box<dyn FnMut(MeetingEvent, &Meeting) + Send + 'static>;

// The C API takes a `void*`, but the watcher is a process-wide singleton anyway,
// so the handler lives here rather than being leaked into a raw pointer.
static MEETING_HANDLER: Mutex<Option<MeetingHandler>> = Mutex::new(None);

unsafe extern "C" fn meeting_trampoline(
    meeting: *const sys::Meeting,
    event: std::os::raw::c_int,
    _user_data: *mut c_void,
) {
    if meeting.is_null() {
        return;
    }
    let event = match event {
        0 => MeetingEvent::Started,
        2 => MeetingEvent::Ended,
        _ => MeetingEvent::Updated,
    };
    let parsed = Meeting::from_raw(&*meeting);

    // A panic must not cross the FFI boundary.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Ok(mut guard) = MEETING_HANDLER.lock() {
            if let Some(handler) = guard.as_mut() {
                handler(event, &parsed);
            }
        }
    }));
}

/// Stops watching when dropped.
#[must_use = "watching stops as soon as this guard is dropped"]
pub struct Watcher {
    _private: (),
}

impl Drop for Watcher {
    fn drop(&mut self) {
        unsafe { sys::mrec_watch_stop() };
        if let Ok(mut guard) = MEETING_HANDLER.lock() {
            *guard = None;
        }
    }
}

/// Watch for meetings starting, changing and ending.
///
/// The handler runs on an internal serial queue, not a realtime thread, so it may
/// allocate and block.
pub fn watch<F>(handler: F) -> Result<Watcher, Error>
where
    F: FnMut(MeetingEvent, &Meeting) + Send + 'static,
{
    if unsafe { sys::mrec_is_watching() } != 0 {
        return Err(Error::AlreadyRunning);
    }
    *MEETING_HANDLER
        .lock()
        .map_err(|e| Error::Internal(e.to_string()))? = Some(Box::new(handler));

    let status = unsafe { sys::mrec_watch_start(Some(meeting_trampoline), std::ptr::null_mut()) };
    if status != sys::MREC_OK {
        *MEETING_HANDLER.lock().unwrap() = None;
        return check(status).map(|_| unreachable!());
    }
    Ok(Watcher { _private: () })
}

/* ---- capture ----------------------------------------------------------- */

/// A block of interleaved float32 PCM.
pub struct AudioBuffer<'a> {
    pub frames: &'a [f32],
    pub channels: u32,
    pub sample_rate: f64,
    pub host_time_ns: u64,
}

type AudioHandler = Box<dyn FnMut(AudioBuffer<'_>) + Send + 'static>;

static AUDIO_HANDLER: Mutex<Option<AudioHandler>> = Mutex::new(None);

/// Realtime audio thread.
///
/// Taking a mutex here is not ideal for a realtime context — it can in principle
/// invert priority against a caller that is swapping the handler. In practice the
/// handler is set once before capture starts and cleared after it stops, so the
/// lock is uncontended. A caller doing real work should still forward into a
/// lock-free queue and return immediately.
unsafe extern "C" fn audio_trampoline(
    frames: *const f32,
    frame_count: u32,
    channels: u32,
    sample_rate: f64,
    host_time_ns: u64,
    _user_data: *mut c_void,
) {
    if frames.is_null() || frame_count == 0 {
        return;
    }
    let len = frame_count as usize * channels.max(1) as usize;
    let slice = std::slice::from_raw_parts(frames, len);

    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Ok(mut guard) = AUDIO_HANDLER.try_lock() {
            if let Some(handler) = guard.as_mut() {
                handler(AudioBuffer {
                    frames: slice,
                    channels,
                    sample_rate,
                    host_time_ns,
                });
            }
        }
    }));
}

/// What to capture.
#[derive(Debug, Clone, Default)]
pub struct CaptureOptions {
    /// Specific processes.
    pub pids: Vec<u32>,
    /// Or the whole system mix. Records silence while the user's output is muted,
    /// so prefer `pids` or [`record`].
    pub system_wide: bool,
    /// Mono mixdown. Defaults to true via [`CaptureOptions::mono`].
    pub stereo: bool,
    /// Also silence the captured processes' own output.
    pub mute_captured_output: bool,
}

/// Stops capture when dropped.
#[must_use = "capture stops as soon as this guard is dropped"]
pub struct Capture {
    pub sample_rate: f64,
    pub channels: u32,
}

impl Drop for Capture {
    fn drop(&mut self) {
        unsafe { sys::mrec_stop() };
        if let Ok(mut guard) = AUDIO_HANDLER.lock() {
            *guard = None;
        }
    }
}

fn install_handler<F>(handler: F) -> Result<(), Error>
where
    F: FnMut(AudioBuffer<'_>) + Send + 'static,
{
    *AUDIO_HANDLER
        .lock()
        .map_err(|e| Error::Internal(e.to_string()))? = Some(Box::new(handler));
    Ok(())
}

fn finish_start(status: i32) -> Result<Capture, Error> {
    if status != sys::MREC_OK {
        *AUDIO_HANDLER.lock().unwrap() = None;
        check(status)?;
    }
    let mut sample_rate = 0.0f64;
    let mut channels = 0u32;
    unsafe { sys::mrec_current_format(&mut sample_rate, &mut channels) };
    Ok(Capture {
        sample_rate,
        channels,
    })
}

/// Capture specific processes, or the system mix.
///
/// The handler runs on a realtime audio thread: do not allocate, lock, or perform
/// I/O in it. Copy into a queue and process elsewhere.
///
/// Blocks for up to six seconds on the first call, while the permission grant is
/// undetermined, so do not call it from a UI thread.
pub fn capture<F>(options: CaptureOptions, handler: F) -> Result<Capture, Error>
where
    F: FnMut(AudioBuffer<'_>) + Send + 'static,
{
    if unsafe { sys::mrec_is_running() } != 0 {
        return Err(Error::AlreadyRunning);
    }
    install_handler(handler)?;

    let status = unsafe {
        sys::mrec_start_raw(
            if options.pids.is_empty() {
                std::ptr::null()
            } else {
                options.pids.as_ptr()
            },
            options.pids.len(),
            options.system_wide as i32,
            !options.stereo as i32,
            options.mute_captured_output as i32,
            Some(audio_trampoline),
            std::ptr::null_mut(),
        )
    };
    finish_start(status)
}

/// Capture a detected meeting. The usual entry point: no pids involved.
pub fn record<F>(meeting: &Meeting, handler: F) -> Result<Capture, Error>
where
    F: FnMut(AudioBuffer<'_>) + Send + 'static,
{
    if unsafe { sys::mrec_is_running() } != 0 {
        return Err(Error::AlreadyRunning);
    }
    install_handler(handler)?;
    let status =
        unsafe { sys::mrec_start_meeting(&meeting.raw, Some(audio_trampoline), std::ptr::null_mut()) };
    finish_start(status)
}

pub fn is_capturing() -> bool {
    unsafe { sys::mrec_is_running() != 0 }
}

pub fn is_watching() -> bool {
    unsafe { sys::mrec_is_watching() != 0 }
}
