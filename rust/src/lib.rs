//! System-audio and microphone capture with meeting detection for macOS and
//! Windows.
//!
//! ```no_run
//! use meeting_record::{meetings, Error, MeetingEvent, Platform};
//!
//! # fn main() -> Result<(), Error> {
//! let watcher = meetings::watch(|event, meeting| {
//!     if event == MeetingEvent::Started && meeting.should_record {
//!         // `platform` is a stable identifier, not a label: match on it.
//!         match meeting.platform {
//!             Platform::Zoom | Platform::Teams => { /* ... */ }
//!             _ => {}
//!         }
//!     }
//! })?;
//! # Ok(())
//! # }
//! ```
//!
//! Capture and watching are process-wide singletons. Both return a guard that
//! stops them on drop.

mod mixed_audio;
mod mixer;
mod sys;

pub use mixed_audio::{MixedAudio, MixedAudioChunk, MixedAudioOptions};

use std::{
    cell::UnsafeCell,
    collections::VecDeque,
    error::Error as StdError,
    ffi::{c_char, c_int, c_void, CStr},
    fmt::{Display, Formatter, Result as FmtResult},
    mem,
    panic::{self, AssertUnwindSafe},
    ptr, slice,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Condvar, Mutex, Weak,
    },
    thread,
    time::Duration,
};

use sys::{AudioCallback as RawAudioCallback, Meeting as RawMeeting, Process as RawProcess};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    UnsupportedOs,
    Permission(String),
    AlreadyRunning,
    Capture(String),
    Internal(String),
    Cancelled,
}

impl Display for Error {
    fn fmt(&self, f: &mut Formatter<'_>) -> FmtResult {
        match self {
            Error::UnsupportedOs => f.write_str("unsupported OS version"),
            Error::Permission(message) => write!(f, "permission not granted: {message}"),
            Error::AlreadyRunning => f.write_str("already running"),
            Error::Capture(message) => write!(f, "capture failed: {message}"),
            Error::Internal(message) => write!(f, "internal error: {message}"),
            Error::Cancelled => f.write_str("operation cancelled"),
        }
    }
}

impl StdError for Error {}

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
        sys::MREC_ERR_PERMISSION => Err(Error::Permission(error_detail(status))),
        sys::MREC_ERR_ALREADY_RUNNING => Err(Error::AlreadyRunning),
        sys::MREC_ERR_NOT_RUNNING
        | sys::MREC_ERR_NO_PROCESSES
        | sys::MREC_ERR_TAP_FAILED
        | sys::MREC_ERR_DEVICE_FAILED
        | sys::MREC_ERR_IOPROC_FAILED => Err(Error::Capture(error_detail(status))),
        _ => Err(Error::Internal(error_detail(status))),
    }
}

fn error_detail(status: i32) -> String {
    let detail = last_error();
    if detail.is_empty() || detail == "no error" {
        default_error_message(status).to_owned()
    } else {
        detail
    }
}

fn default_error_message(status: i32) -> &'static str {
    match status {
        sys::MREC_ERR_UNSUPPORTED_OS => "unsupported OS version",
        sys::MREC_ERR_PERMISSION => "permission not granted",
        sys::MREC_ERR_ALREADY_RUNNING => "already running",
        sys::MREC_ERR_NOT_RUNNING => "not running",
        sys::MREC_ERR_NO_PROCESSES => "no process to capture",
        sys::MREC_ERR_TAP_FAILED => "audio tap failed",
        sys::MREC_ERR_DEVICE_FAILED => "audio device failed",
        sys::MREC_ERR_IOPROC_FAILED => "audio callback failed",
        _ => "meeting-record failed",
    }
}

fn c_string(bytes: &[c_char]) -> String {
    unsafe {
        CStr::from_ptr(bytes.as_ptr())
            .to_string_lossy()
            .into_owned()
    }
}

/// Shared cancellation for capture and permission waits. Cancellation is permanent.
#[derive(Clone, Default)]
pub struct CancellationToken {
    inner: Arc<CancellationState>,
}

#[derive(Default)]
struct CancellationState {
    data: Mutex<CancellationData>,
    wake: Condvar,
}

#[derive(Default)]
struct CancellationData {
    cancelled: bool,
    capture: Weak<CaptureControl>,
}

impl std::fmt::Debug for CancellationToken {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> FmtResult {
        formatter
            .debug_struct("CancellationToken")
            .field("cancelled", &self.is_cancelled())
            .finish()
    }
}

impl CancellationToken {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn cancel(&self) {
        let capture = {
            let mut data = self
                .inner
                .data
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            data.cancelled = true;
            self.inner.wake.notify_all();
            mem::take(&mut data.capture).upgrade()
        };
        if let Some(capture) = capture {
            capture.stop();
        }
    }

    pub fn is_cancelled(&self) -> bool {
        self.inner
            .data
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .cancelled
    }

    fn check(&self) -> Result<(), Error> {
        if self.is_cancelled() {
            Err(Error::Cancelled)
        } else {
            Ok(())
        }
    }

    fn register(&self, capture: &Arc<CaptureControl>) -> Result<(), Error> {
        let mut data = self
            .inner
            .data
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if data.cancelled {
            return Err(Error::Cancelled);
        }
        data.capture = Arc::downgrade(capture);
        Ok(())
    }

    fn wait(&self, timeout: Duration) -> Result<(), Error> {
        let data = self
            .inner
            .data
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let (data, _) = self
            .inner
            .wake
            .wait_timeout_while(data, timeout, |data| !data.cancelled)
            .unwrap_or_else(|error| error.into_inner());
        if data.cancelled {
            Err(Error::Cancelled)
        } else {
            Ok(())
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Permission {
    SystemAudio,
    Microphone,
    Accessibility,
}

/// Windows has no prompts: system audio and accessibility are always granted,
/// and the microphone follows its privacy settings.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PermissionStatus {
    Unknown,
    Granted,
    Denied,
}

impl From<i32> for PermissionStatus {
    fn from(value: i32) -> Self {
        match value {
            1 => PermissionStatus::Granted,
            2 => PermissionStatus::Denied,
            _ => PermissionStatus::Unknown,
        }
    }
}

pub mod permissions {
    use std::{
        thread,
        time::{Duration, Instant},
    };

    use super::{sys, CancellationToken, Error, Permission, PermissionStatus};

    pub fn status(permission: Permission) -> PermissionStatus {
        match permission {
            Permission::SystemAudio => unsafe { sys::mrec_audio_permission_status() }.into(),
            Permission::Microphone => unsafe { sys::mrec_microphone_permission_status() }.into(),
            Permission::Accessibility => {
                unsafe { sys::mrec_accessibility_permission_status() }.into()
            }
        }
    }

    /// Show the system prompt for a permission.
    ///
    /// Audio requests return immediately; poll [`status`] for the answer.
    /// Accessibility opens System Settings and takes effect after an app
    /// relaunch.
    pub fn request(permission: Permission) -> Result<(), Error> {
        let status = match permission {
            Permission::SystemAudio => unsafe { sys::mrec_request_audio_permission() },
            Permission::Microphone => unsafe { sys::mrec_request_microphone_permission() },
            Permission::Accessibility => unsafe { sys::mrec_request_accessibility_permission() },
        };
        super::check(status)
    }

    /// Request and wait for the answer, returning the current status on timeout.
    /// Accessibility returns immediately; its grant requires an app relaunch.
    pub fn request_and_wait(
        permission: Permission,
        timeout: Duration,
        cancellation: Option<&CancellationToken>,
    ) -> Result<PermissionStatus, Error> {
        if let Some(token) = cancellation {
            token.check()?;
        }
        let current = status(permission);
        if current == PermissionStatus::Granted {
            return Ok(current);
        }
        request(permission)?;
        if permission == Permission::Accessibility {
            return Ok(status(permission));
        }
        let start = Instant::now();
        while start.elapsed() < timeout {
            let delay = Duration::from_millis(500).min(timeout.saturating_sub(start.elapsed()));
            if let Some(token) = cancellation {
                token.wait(delay)?;
            } else {
                thread::sleep(delay);
            }
            let current = status(permission);
            if current != PermissionStatus::Unknown {
                return Ok(current);
            }
        }
        if let Some(token) = cancellation {
            token.check()?;
        }
        Ok(status(permission))
    }
}

#[derive(Debug, Clone)]
pub struct AudioProcess {
    pub pid: u32,
    pub name: String,
    pub bundle_id: String,
    pub is_playing_audio: bool,
    pub is_using_mic: bool,
}

impl From<&RawProcess> for AudioProcess {
    fn from(raw: &RawProcess) -> Self {
        Self {
            pid: raw.pid,
            name: c_string(&raw.name),
            bundle_id: c_string(&raw.bundle_id),
            is_playing_audio: raw.is_running_output != 0,
            is_using_mic: raw.is_running_input != 0,
        }
    }
}

/// Every live process doing audio IO.
fn list_audio_processes() -> Result<Vec<AudioProcess>, Error> {
    collect_processes(sys::mrec_list_audio_processes)
}

/// Applications doing audio IO, with helper processes folded into their app.
fn list_audio_apps() -> Result<Vec<AudioProcess>, Error> {
    collect_processes(sys::mrec_list_audio_apps)
}

fn collect_processes(
    list: unsafe extern "C" fn(*mut RawProcess, usize, *mut usize) -> std::os::raw::c_int,
) -> Result<Vec<AudioProcess>, Error> {
    let mut buffer = vec![
        RawProcess {
            pid: 0,
            is_running_output: 0,
            is_running_input: 0,
            bundle_id: [0; 256],
            name: [0; 256],
        };
        256
    ];
    let mut count = 0usize;
    loop {
        let status = unsafe { list(buffer.as_mut_ptr(), buffer.len(), &mut count) };
        if status == sys::MREC_ERR_BUFFER_TOO_SMALL {
            buffer.resize(buffer.len() * 2, unsafe { mem::zeroed::<RawProcess>() });
            continue;
        }
        check(status)?;
        return Ok(buffer.iter().take(count).map(AudioProcess::from).collect());
    }
}

/// Conferencing platform with a stable [`Platform::as_str`] identifier.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum Platform {
    Unknown,
    Zoom,
    Teams,
    Meet,
    Webex,
    Slack,
    Discord,
    /// A browser tab on a call URL we recognise but cannot attribute further.
    Browser,
}

impl From<i32> for Platform {
    fn from(value: i32) -> Self {
        match value {
            1 => Platform::Zoom,
            2 => Platform::Teams,
            3 => Platform::Meet,
            4 => Platform::Webex,
            5 => Platform::Slack,
            6 => Platform::Discord,
            7 => Platform::Browser,
            _ => Platform::Unknown,
        }
    }
}

impl Platform {
    /// Stable identifier: `"zoom"`, `"teams"`, `"meet"`, `"webex"`, `"slack"`,
    /// `"discord"`, `"browser"`, `"unknown"`.
    pub const fn as_str(self) -> &'static str {
        match self {
            Platform::Unknown => "unknown",
            Platform::Zoom => "zoom",
            Platform::Teams => "teams",
            Platform::Meet => "meet",
            Platform::Webex => "webex",
            Platform::Slack => "slack",
            Platform::Discord => "discord",
            Platform::Browser => "browser",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Meeting {
    pub platform: Platform,
    /// The user-visible application. Use this with [`CaptureTarget::Process`].
    pub pid: u32,
    pub app_name: String,
    /// Window or tab title. Requires the accessibility permission on macOS;
    /// empty otherwise.
    pub title: String,
    /// The active tab's address for browser meetings. Requires the
    /// accessibility permission on macOS; empty otherwise, or when the call
    /// is in a background tab.
    pub url: String,
    pub is_using_mic: bool,
    pub is_playing_audio: bool,
    pub confidence: i32,
    /// Prefer this to comparing `confidence`; the weights may change.
    pub should_record: bool,
}

impl From<&RawMeeting> for Meeting {
    fn from(raw: &RawMeeting) -> Self {
        Self {
            platform: raw.platform.into(),
            pid: raw.pid,
            app_name: c_string(&raw.app_name),
            title: c_string(&raw.title),
            url: c_string(&raw.url),
            is_using_mic: raw.is_using_mic != 0,
            is_playing_audio: raw.is_playing_audio != 0,
            confidence: raw.confidence,
            should_record: raw.should_record != 0,
        }
    }
}

/// Point-in-time scan, highest confidence first.
fn scan_meetings() -> Result<Vec<Meeting>, Error> {
    let mut buffer = vec![unsafe { mem::zeroed::<RawMeeting>() }; 16];
    let mut count = 0usize;
    loop {
        let status = unsafe { sys::mrec_scan(buffer.as_mut_ptr(), buffer.len(), &mut count) };
        if status == sys::MREC_ERR_BUFFER_TOO_SMALL {
            buffer.resize(buffer.len() * 2, unsafe { mem::zeroed::<RawMeeting>() });
            continue;
        }
        check(status)?;
        return Ok(buffer.iter().take(count).map(Meeting::from).collect());
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MeetingEvent {
    Started,
    Updated,
    Ended,
}

impl From<i32> for MeetingEvent {
    fn from(value: i32) -> Self {
        match value {
            0 => MeetingEvent::Started,
            2 => MeetingEvent::Ended,
            _ => MeetingEvent::Updated,
        }
    }
}

type MeetingHandler = Box<dyn FnMut(MeetingEvent, &Meeting) + Send + 'static>;

// The watcher is a process-wide singleton, so the handler lives here rather
// than in the C API's `void*`.
struct MeetingHandlerSlot {
    handler: Option<MeetingHandler>,
    generation: u64,
}

static MEETING_HANDLER: Mutex<MeetingHandlerSlot> = Mutex::new(MeetingHandlerSlot {
    handler: None,
    generation: 0,
});

unsafe extern "C" fn meeting_trampoline(
    meeting: *const RawMeeting,
    event: c_int,
    _user_data: *mut c_void,
) {
    if meeting.is_null() {
        return;
    }
    let event = event.into();
    let parsed = Meeting::from(&*meeting);

    let (handler, generation) = match MEETING_HANDLER.lock() {
        Ok(mut slot) => (slot.handler.take(), slot.generation),
        Err(_) => return,
    };
    let Some(mut handler) = handler else { return };

    // A panic must not cross the FFI boundary. Invoke without holding the slot
    // lock so a handler can drop its own watcher without deadlocking.
    let result = panic::catch_unwind(AssertUnwindSafe(|| handler(event, &parsed)));
    if result.is_ok() {
        if let Ok(mut slot) = MEETING_HANDLER.lock() {
            if slot.generation == generation && slot.handler.is_none() {
                slot.handler = Some(handler);
            }
        }
    }
}

/// Stops watching when dropped.
#[must_use = "watching stops when this guard is dropped"]
pub struct Watcher {
    _private: (),
}

impl Drop for Watcher {
    fn drop(&mut self) {
        unsafe { sys::mrec_watch_stop() };
        if let Ok(mut slot) = MEETING_HANDLER.lock() {
            slot.generation = slot.generation.wrapping_add(1);
            slot.handler = None;
        }
    }
}

/// Watch for meetings starting, changing and ending.
///
/// The handler runs on an internal serial queue, so it may allocate and block.
fn watch_meetings<F>(handler: F) -> Result<Watcher, Error>
where
    F: FnMut(MeetingEvent, &Meeting) + Send + 'static,
{
    let mut slot = MEETING_HANDLER
        .lock()
        .map_err(|error| Error::Internal(error.to_string()))?;
    if slot.handler.is_some() || unsafe { sys::mrec_is_watching() } != 0 {
        return Err(Error::AlreadyRunning);
    }
    slot.generation = slot.generation.wrapping_add(1);
    slot.handler = Some(Box::new(handler));

    let status = unsafe { sys::mrec_watch_start(Some(meeting_trampoline), ptr::null_mut()) };
    if status != sys::MREC_OK {
        slot.generation = slot.generation.wrapping_add(1);
        slot.handler = None;
        return check(status).map(|_| unreachable!());
    }
    drop(slot);
    Ok(Watcher { _private: () })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureTarget {
    /// A detected meeting's app pid, or any audio-producing process pid.
    Process { pid: u32 },
    /// All application output.
    System,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MicrophoneSource {
    /// Follow the OS default input device.
    Default,
    /// Prefer the captured app's microphone on Windows, then its communications
    /// default. On macOS, follow the default input device.
    Auto,
}

#[derive(Debug, Clone)]
pub struct CaptureOptions {
    /// Mono mixdown for system audio. Defaults to true.
    pub mono: bool,
    /// Optional microphone track. Defaults to none.
    pub microphone: Option<MicrophoneSource>,
    /// Additional timestamp-aligned mono s16le output.
    pub mix: Option<MixedAudioOptions>,
    /// Cancellation stops capture, including a pending start.
    pub cancellation: Option<CancellationToken>,
}

impl Default for CaptureOptions {
    fn default() -> Self {
        Self {
            mono: true,
            microphone: None,
            mix: None,
            cancellation: None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureState {
    /// Audio frames are available from the tracks.
    Recording,
    /// Incoming frames are discarded. No silence is synthesized.
    Paused,
    Stopped,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Track {
    SystemAudio,
    Microphone,
}

/// Reason for source loss.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum InterruptionReason {
    /// Unplugged, disabled, or none left.
    DeviceRemoved,
    /// The device in use changed and did not reopen.
    DeviceChanged,
    FormatChanged,
    /// Every captured app exited.
    AppExited,
    /// The audio service restarted.
    ServiceReset,
    Unknown,
}

impl From<i32> for InterruptionReason {
    fn from(value: i32) -> Self {
        match value {
            sys::MREC_TRACK_REASON_DEVICE_REMOVED => InterruptionReason::DeviceRemoved,
            sys::MREC_TRACK_REASON_DEVICE_CHANGED => InterruptionReason::DeviceChanged,
            sys::MREC_TRACK_REASON_FORMAT_CHANGED => InterruptionReason::FormatChanged,
            sys::MREC_TRACK_REASON_APP_EXITED => InterruptionReason::AppExited,
            sys::MREC_TRACK_REASON_SERVICE_RESET => InterruptionReason::ServiceReset,
            _ => InterruptionReason::Unknown,
        }
    }
}

/// Source availability changes independently of recording pause and stop.
/// Lost devices are retried; exited apps require selecting a new process target.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum TrackEvent {
    /// No source is available; `Degraded` or `Resumed` reports recovery.
    Interrupted {
        track: Track,
        reason: InterruptionReason,
        message: String,
    },
    Resumed {
        track: Track,
    },
    /// Some sources are unavailable; the others continue delivering audio.
    Degraded {
        track: Track,
        reason: InterruptionReason,
        message: String,
    },
    /// The microphone moved to another device.
    DeviceChanged {
        track: Track,
        device_name: String,
    },
}

/// What a track's events have said so far.
#[derive(Default)]
struct TrackStatus {
    availability: AtomicUsize,
    device_name: Mutex<Option<String>>,
}

/// Track events in arrival order, until capture stops.
#[derive(Default)]
struct EventQueue {
    events: Mutex<VecDeque<TrackEvent>>,
    ready: Condvar,
    closed: AtomicBool,
}

impl EventQueue {
    fn push(&self, event: TrackEvent) {
        self.events
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .push_back(event);
        self.ready.notify_all();
    }

    fn close(&self) {
        let _events = self
            .events
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        self.closed.store(true, Ordering::Release);
        self.ready.notify_all();
    }

    fn recv(&self) -> Option<TrackEvent> {
        let mut events = self
            .events
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        loop {
            if let Some(event) = events.pop_front() {
                return Some(event);
            }
            if self.closed.load(Ordering::Acquire) {
                return None;
            }
            events = self
                .ready
                .wait(events)
                .unwrap_or_else(|error| error.into_inner());
        }
    }

    fn try_recv(&self) -> Option<TrackEvent> {
        self.events
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .pop_front()
    }
}

struct EventContext {
    queue: Arc<EventQueue>,
    system_audio: Arc<TrackStatus>,
    microphone: Arc<TrackStatus>,
}

unsafe extern "C" fn track_event_trampoline(event: *const sys::TrackEvent, user_data: *mut c_void) {
    if event.is_null() || user_data.is_null() {
        return;
    }
    let context = &*(user_data as *const EventContext);
    let event = &*event;
    let (track, status) = if event.track == sys::MREC_TRACK_MICROPHONE {
        (Track::Microphone, &context.microphone)
    } else {
        (Track::SystemAudio, &context.system_audio)
    };
    let event = match event.kind {
        sys::MREC_TRACK_INTERRUPTED => {
            status.availability.store(2, Ordering::Release);
            TrackEvent::Interrupted {
                track,
                reason: event.reason.into(),
                message: c_string(&event.message),
            }
        }
        sys::MREC_TRACK_RESUMED => {
            status.availability.store(0, Ordering::Release);
            TrackEvent::Resumed { track }
        }
        sys::MREC_TRACK_DEGRADED => {
            status.availability.store(1, Ordering::Release);
            TrackEvent::Degraded {
                track,
                reason: event.reason.into(),
                message: c_string(&event.message),
            }
        }
        sys::MREC_TRACK_DEVICE_CHANGED => {
            let device_name = c_string(&event.device_name);
            *status
                .device_name
                .lock()
                .unwrap_or_else(|error| error.into_inner()) = Some(device_name.clone());
            TrackEvent::DeviceChanged { track, device_name }
        }
        _ => return,
    };
    context.queue.push(event);
}

#[derive(Debug)]
pub struct AudioChunk {
    /// Interleaved float32 PCM.
    pub frames: Vec<f32>,
    /// Capture time of the first frame on the host's monotonic clock.
    pub host_time_ns: u64,
    pub frame_count: u32,
    pub channels: u32,
    pub sample_rate: f64,
    /// Samples discarded since the previous delivered chunk.
    pub dropped_samples: usize,
}

const TRACK_CAPACITY: usize = 1 << 20;
const TRACK_PACKET_CAPACITY: usize = 1 << 12;
// Low bits hold the phase; upper bits identify segments across pause/resume.
const STATE_RECORDING: usize = 0;
const STATE_PAUSED: usize = 1;
const STATE_STOPPED: usize = 2;
const STATE_MASK: usize = 3;

#[derive(Clone, Copy, Default)]
struct PacketMeta {
    sample_start: usize,
    sample_count: usize,
    frame_count: u32,
    channels: u32,
    sample_rate: f64,
    host_time_ns: u64,
    dropped_before: usize,
    segment: usize,
}

struct QueuedChunk {
    audio: AudioChunk,
    segment: usize,
}

struct TrackQueue {
    buffer: Box<[UnsafeCell<f32>]>,
    mask: usize,
    packets: Box<[UnsafeCell<PacketMeta>]>,
    packet_mask: usize,
    head: AtomicUsize,
    tail: AtomicUsize,
    packet_head: AtomicUsize,
    packet_tail: AtomicUsize,
    dropped: AtomicUsize,
    dropped_since_read: AtomicUsize,
    writing: AtomicBool,
    closed: AtomicBool,
    read_lock: Mutex<()>,
    wake_lock: Mutex<()>,
    wake: Condvar,
}

// SAFETY: producers are serialized by `writing`, consumers by `read_lock`, and
// published regions are transferred with the atomic head and tail positions.
unsafe impl Sync for TrackQueue {}

impl TrackQueue {
    fn new() -> Self {
        let buffer = (0..TRACK_CAPACITY)
            .map(|_| UnsafeCell::new(0.0))
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Self {
            buffer,
            mask: TRACK_CAPACITY - 1,
            packets: (0..TRACK_PACKET_CAPACITY)
                .map(|_| UnsafeCell::new(PacketMeta::default()))
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            packet_mask: TRACK_PACKET_CAPACITY - 1,
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
            packet_head: AtomicUsize::new(0),
            packet_tail: AtomicUsize::new(0),
            dropped: AtomicUsize::new(0),
            dropped_since_read: AtomicUsize::new(0),
            writing: AtomicBool::new(false),
            closed: AtomicBool::new(false),
            read_lock: Mutex::new(()),
            wake_lock: Mutex::new(()),
            wake: Condvar::new(),
        }
    }

    fn write(&self, frames: &[f32], mut packet: PacketMeta, state: &AtomicUsize) {
        if self
            .writing
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            self.dropped.fetch_add(frames.len(), Ordering::Relaxed);
            self.dropped_since_read
                .fetch_add(frames.len(), Ordering::Relaxed);
            return;
        }

        if self.closed.load(Ordering::Acquire)
            || state.load(Ordering::Acquire) != packet.segment << 2
        {
            self.writing.store(false, Ordering::Release);
            return;
        }

        let head = self.head.load(Ordering::Relaxed);
        let tail = self.tail.load(Ordering::Acquire);
        let used = head.wrapping_sub(tail);
        let packet_head = self.packet_head.load(Ordering::Relaxed);
        let packet_tail = self.packet_tail.load(Ordering::Acquire);
        if !frames.is_empty()
            && frames.len() <= TRACK_CAPACITY.saturating_sub(used)
            && packet_head.wrapping_sub(packet_tail) < TRACK_PACKET_CAPACITY
        {
            for (offset, sample) in frames.iter().enumerate() {
                unsafe { *self.buffer[(head + offset) & self.mask].get() = *sample };
            }
            packet.sample_start = head;
            packet.sample_count = frames.len();
            packet.dropped_before = self.dropped_since_read.swap(0, Ordering::Relaxed);
            unsafe {
                *self.packets[packet_head & self.packet_mask].get() = packet;
            }
            self.head
                .store(head.wrapping_add(frames.len()), Ordering::Release);
            self.packet_head
                .store(packet_head.wrapping_add(1), Ordering::Release);
            self.wake.notify_one();
        } else {
            self.dropped.fetch_add(frames.len(), Ordering::Relaxed);
            self.dropped_since_read
                .fetch_add(frames.len(), Ordering::Relaxed);
        }
        self.writing.store(false, Ordering::Release);
    }

    fn try_recv(&self) -> Option<QueuedChunk> {
        let _read = self
            .read_lock
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let packet_tail = self.packet_tail.load(Ordering::Relaxed);
        let packet_head = self.packet_head.load(Ordering::Acquire);
        if packet_tail == packet_head {
            return None;
        }
        let first = unsafe { *self.packets[packet_tail & self.packet_mask].get() };
        let mut next_packet = packet_tail;
        let mut sample_count = 0usize;
        let mut frame_count = 0u32;
        while next_packet != packet_head {
            let packet = unsafe { *self.packets[next_packet & self.packet_mask].get() };
            if packet.channels != first.channels
                || packet.sample_rate != first.sample_rate
                || packet.segment != first.segment
                || (next_packet != packet_tail
                    && (packet.dropped_before != 0
                        || !Self::packets_are_contiguous(&first, frame_count, &packet)))
            {
                break;
            }
            sample_count += packet.sample_count;
            frame_count = frame_count.saturating_add(packet.frame_count);
            next_packet = next_packet.wrapping_add(1);
        }
        let mut frames = Vec::with_capacity(sample_count);
        for offset in 0..sample_count {
            frames.push(unsafe { *self.buffer[(first.sample_start + offset) & self.mask].get() });
        }
        self.tail.store(
            first.sample_start.wrapping_add(sample_count),
            Ordering::Release,
        );
        self.packet_tail.store(next_packet, Ordering::Release);
        Some(QueuedChunk {
            audio: AudioChunk {
                frames,
                host_time_ns: first.host_time_ns,
                frame_count,
                channels: first.channels,
                sample_rate: first.sample_rate,
                dropped_samples: first.dropped_before,
            },
            segment: first.segment,
        })
    }

    fn recv(&self) -> Option<AudioChunk> {
        loop {
            // Closure precedes the final read so EOF includes the last publication.
            let closed = self.closed.load(Ordering::Acquire);
            if let Some(packet) = self.try_recv() {
                return Some(packet.audio);
            }
            if closed {
                return None;
            }
            let guard = self
                .wake_lock
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            let _ = self
                .wake
                .wait_timeout(guard, Duration::from_millis(100))
                .unwrap_or_else(|error| error.into_inner());
        }
    }

    fn barrier(&self) {
        self.wait_for_writer();
        self.writing.store(false, Ordering::Release);
    }

    fn wait_for_writer(&self) {
        while self
            .writing
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            thread::yield_now();
        }
    }

    fn clear(&self) {
        self.wait_for_writer();
        let _read = self
            .read_lock
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        self.tail
            .store(self.head.load(Ordering::Acquire), Ordering::Release);
        self.packet_tail
            .store(self.packet_head.load(Ordering::Acquire), Ordering::Release);
        self.dropped_since_read.store(0, Ordering::Relaxed);
        self.writing.store(false, Ordering::Release);
    }

    fn packets_are_contiguous(
        first: &PacketMeta,
        preceding_frames: u32,
        next: &PacketMeta,
    ) -> bool {
        if first.host_time_ns == 0 || next.host_time_ns == 0 || first.sample_rate <= 0.0 {
            return true;
        }
        let elapsed =
            (f64::from(preceding_frames) * 1_000_000_000.0 / first.sample_rate).round() as u64;
        let expected = first.host_time_ns.saturating_add(elapsed);
        expected.abs_diff(next.host_time_ns) <= 2_000_000
    }

    fn close(&self) {
        self.wait_for_writer();
        self.closed.store(true, Ordering::Release);
        self.writing.store(false, Ordering::Release);
        self.wake.notify_all();
    }
}

/// Availability of a track's sources, independent of pause and stop.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackAvailability {
    Live,
    Degraded,
    Interrupted,
}

/// One interleaved float32 PCM track.
pub struct AudioTrack {
    sample_rate: f64,
    channels: u32,
    queue: Arc<TrackQueue>,
    status: Arc<TrackStatus>,
}

impl AudioTrack {
    /// Initial negotiated rate. A recovered device may change it; each
    /// [`AudioChunk::sample_rate`] is authoritative.
    pub fn sample_rate(&self) -> f64 {
        self.sample_rate
    }

    /// Initial channel count. A recovered device may change it; each
    /// [`AudioChunk::channels`] is authoritative.
    pub fn channels(&self) -> u32 {
        self.channels
    }

    /// Number of interleaved samples discarded because the bounded queue could
    /// not accept them.
    pub fn dropped_samples(&self) -> usize {
        self.queue.dropped.load(Ordering::Relaxed)
    }

    /// Wait for the next chunk, or return `None` after the recording stops.
    pub fn recv(&self) -> Option<AudioChunk> {
        self.queue.recv()
    }

    /// True when none of this track's sources are available.
    pub fn is_interrupted(&self) -> bool {
        self.availability() == TrackAvailability::Interrupted
    }

    pub fn availability(&self) -> TrackAvailability {
        match self.status.availability.load(Ordering::Acquire) {
            1 => TrackAvailability::Degraded,
            2 => TrackAvailability::Interrupted,
            _ => TrackAvailability::Live,
        }
    }

    /// The microphone in use; `None` for system audio.
    pub fn device_name(&self) -> Option<String> {
        self.status
            .device_name
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .clone()
    }
}

struct TrackCallbackContext {
    queue: Arc<TrackQueue>,
    mix_queue: Option<Arc<TrackQueue>>,
    state: Arc<AtomicUsize>,
}

struct CaptureCallbackContexts {
    system_audio: Box<TrackCallbackContext>,
    microphone: Option<Box<TrackCallbackContext>>,
    events: Box<EventContext>,
}

static CAPTURE_CONTEXTS: Mutex<Option<CaptureCallbackContexts>> = Mutex::new(None);

unsafe extern "C" fn audio_trampoline(
    frames: *const f32,
    frame_count: u32,
    channels: u32,
    sample_rate: f64,
    host_time_ns: u64,
    user_data: *mut c_void,
) {
    if frames.is_null() || frame_count == 0 || user_data.is_null() {
        return;
    }
    let context = &*(user_data as *const TrackCallbackContext);
    let state = context.state.load(Ordering::Acquire);
    if state & STATE_MASK != STATE_RECORDING {
        return;
    }
    let len = frame_count as usize * channels.max(1) as usize;
    let samples = slice::from_raw_parts(frames, len);
    let packet = PacketMeta {
        frame_count,
        channels,
        sample_rate,
        host_time_ns,
        segment: state >> 2,
        ..PacketMeta::default()
    };
    context.queue.write(samples, packet, &context.state);
    if let Some(queue) = &context.mix_queue {
        queue.write(samples, packet, &context.state);
    }
}

struct CaptureControl {
    state: Arc<AtomicUsize>,
    queues: Vec<Arc<TrackQueue>>,
    raw_queues: Vec<Arc<TrackQueue>>,
    events: Arc<EventQueue>,
    lifecycle: Mutex<()>,
}

impl CaptureControl {
    fn phase(&self) -> usize {
        self.state.load(Ordering::Acquire) & STATE_MASK
    }

    fn pause(&self) {
        let _guard = self
            .lifecycle
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if self.phase() != STATE_RECORDING {
            return;
        }
        self.state.fetch_or(STATE_PAUSED, Ordering::AcqRel);
        for queue in &self.queues {
            queue.barrier();
        }
        for queue in &self.raw_queues {
            queue.clear();
        }
    }

    fn resume(&self) {
        let _guard = self
            .lifecycle
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let state = self.state.load(Ordering::Acquire);
        if state & STATE_MASK != STATE_PAUSED {
            return;
        }
        for queue in &self.raw_queues {
            queue.clear();
        }
        self.state
            .store((state & !STATE_MASK).wrapping_add(4), Ordering::Release);
    }

    fn stop(&self) {
        self.stop_with(|| {
            unsafe { sys::mrec_stop() };
        });
    }

    fn stop_with(&self, stop_native: impl FnOnce()) {
        let _guard = self
            .lifecycle
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let state = self.state.load(Ordering::Acquire);
        if state & STATE_MASK == STATE_STOPPED {
            return;
        }
        // Accept final callbacks until native teardown finishes.
        stop_native();
        self.state
            .store((state & !STATE_MASK) | STATE_STOPPED, Ordering::Release);
        for queue in &self.queues {
            queue.close();
        }
        self.events.close();
        *CAPTURE_CONTEXTS
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = None;
    }
}

/// Owns the audio tracks and their shared recording lifecycle.
#[must_use = "capture stops when this session is dropped"]
pub struct CaptureSession {
    system_audio: AudioTrack,
    microphone: Option<AudioTrack>,
    mixed_audio: Option<MixedAudio>,
    control: Arc<CaptureControl>,
}

impl CaptureSession {
    pub fn system_audio(&self) -> &AudioTrack {
        &self.system_audio
    }
    pub fn microphone(&self) -> Option<&AudioTrack> {
        self.microphone.as_ref()
    }
    pub fn mixed_audio(&self) -> Option<&MixedAudio> {
        self.mixed_audio.as_ref()
    }

    pub fn state(&self) -> CaptureState {
        match self.control.phase() {
            STATE_PAUSED => CaptureState::Paused,
            STATE_STOPPED => CaptureState::Stopped,
            _ => CaptureState::Recording,
        }
    }

    /// Waits for the next source change, or returns `None` once capture stops.
    pub fn recv_event(&self) -> Option<TrackEvent> {
        self.control.events.recv()
    }

    /// The next source change, if one is waiting.
    pub fn try_recv_event(&self) -> Option<TrackEvent> {
        self.control.events.try_recv()
    }

    /// Discard incoming frames until resume; paused time is omitted.
    pub fn pause(&self) {
        self.control.pause();
    }
    pub fn resume(&self) {
        self.control.resume();
    }

    /// Stop capture. Tracks retain their queued tail until drained.
    pub fn stop(&self) {
        self.control.stop();
    }
}

impl Drop for CaptureSession {
    fn drop(&mut self) {
        self.stop();
    }
}

fn start_capture(target: CaptureTarget, options: CaptureOptions) -> Result<CaptureSession, Error> {
    if let Some(token) = &options.cancellation {
        token.check()?;
    }
    if options.mix.is_some_and(|mix| mix.sample_rate == 0) {
        return Err(Error::Capture(
            "mix sample rate must be non-zero".to_owned(),
        ));
    }
    if unsafe { sys::mrec_is_running() } != 0 {
        return Err(Error::AlreadyRunning);
    }
    let (pids, system_wide) = match target {
        CaptureTarget::Process { pid: 0 } => {
            return Err(Error::Capture("process pid must be non-zero".to_owned()))
        }
        CaptureTarget::Process { pid } => (vec![pid], false),
        CaptureTarget::System => (Vec::new(), true),
    };
    let state = Arc::new(AtomicUsize::new(STATE_RECORDING));
    let system_queue = Arc::new(TrackQueue::new());
    let microphone_queue = options.microphone.map(|_| Arc::new(TrackQueue::new()));
    let events = Arc::new(EventQueue::default());
    let system_status = Arc::new(TrackStatus::default());
    let microphone_status = Arc::new(TrackStatus::default());
    let system_mix_queue = options.mix.map(|_| Arc::new(TrackQueue::new()));
    let microphone_mix_queue = options
        .mix
        .and(options.microphone)
        .map(|_| Arc::new(TrackQueue::new()));
    let raw_queues: Vec<_> = std::iter::once(Arc::clone(&system_queue))
        .chain(microphone_queue.iter().cloned())
        .collect();
    let mix_queues: Vec<_> = system_mix_queue
        .iter()
        .cloned()
        .chain(microphone_mix_queue.iter().cloned())
        .collect();
    let mut contexts = CaptureCallbackContexts {
        system_audio: Box::new(TrackCallbackContext {
            queue: Arc::clone(&system_queue),
            mix_queue: system_mix_queue,
            state: Arc::clone(&state),
        }),
        microphone: microphone_queue.as_ref().map(|queue| {
            Box::new(TrackCallbackContext {
                queue: Arc::clone(queue),
                mix_queue: microphone_mix_queue,
                state: Arc::clone(&state),
            })
        }),
        events: Box::new(EventContext {
            queue: Arc::clone(&events),
            system_audio: Arc::clone(&system_status),
            microphone: Arc::clone(&microphone_status),
        }),
    };
    let system_context = (&mut *contexts.system_audio) as *mut TrackCallbackContext as *mut c_void;
    let microphone_context = contexts
        .microphone
        .as_mut()
        .map(|context| (&mut **context) as *mut TrackCallbackContext as *mut c_void)
        .unwrap_or(ptr::null_mut());
    let event_context = (&mut *contexts.events) as *mut EventContext as *mut c_void;
    let mut context_slot = CAPTURE_CONTEXTS
        .lock()
        .map_err(|error| Error::Internal(error.to_string()))?;
    if context_slot.is_some() {
        return Err(Error::AlreadyRunning);
    }
    *context_slot = Some(contexts);
    drop(context_slot);
    let control = Arc::new(CaptureControl {
        state: Arc::clone(&state),
        queues: raw_queues
            .iter()
            .cloned()
            .chain(mix_queues.iter().cloned())
            .collect(),
        raw_queues,
        events,
        lifecycle: Mutex::new(()),
    });
    let result = (|| {
        let status = unsafe {
            sys::mrec_start_tracks_raw(
                if pids.is_empty() {
                    ptr::null()
                } else {
                    pids.as_ptr()
                },
                pids.len(),
                system_wide as i32,
                options.mono as i32,
                0,
                match options.microphone {
                    Some(MicrophoneSource::Default) => 1,
                    Some(MicrophoneSource::Auto) => 2,
                    None => 0,
                },
                Some(audio_trampoline),
                system_context,
                options
                    .microphone
                    .map(|_| audio_trampoline as RawAudioCallback),
                microphone_context,
                Some(track_event_trampoline),
                event_context,
            )
        };
        if let Some(token) = &options.cancellation {
            token.check()?;
        }
        check(status)?;
        let mut system_sample_rate = 0.0;
        let mut system_channels = 0;
        check(unsafe { sys::mrec_current_format(&mut system_sample_rate, &mut system_channels) })?;
        let microphone = if let Some(queue) = microphone_queue {
            let mut sample_rate = 0.0;
            let mut channels = 0;
            check(unsafe { sys::mrec_current_microphone_format(&mut sample_rate, &mut channels) })?;
            let mut name = [0 as c_char; 256];
            if unsafe { sys::mrec_current_microphone_name(name.as_mut_ptr(), name.len()) }
                == sys::MREC_OK
            {
                *microphone_status
                    .device_name
                    .lock()
                    .unwrap_or_else(|error| error.into_inner()) = Some(c_string(&name));
            }
            Some(AudioTrack {
                sample_rate,
                channels,
                queue,
                status: microphone_status,
            })
        } else {
            None
        };
        let mixed_audio = options
            .mix
            .map(|mix| MixedAudio::new(mix.sample_rate, mix_queues, state))
            .transpose()?;
        if let Some(token) = &options.cancellation {
            token.register(&control)?;
        }
        Ok(CaptureSession {
            system_audio: AudioTrack {
                sample_rate: system_sample_rate,
                channels: system_channels,
                queue: system_queue,
                status: system_status,
            },
            microphone,
            mixed_audio,
            control: Arc::clone(&control),
        })
    })();
    if result.is_err() {
        control.stop();
    }
    result
}

fn capture_running() -> bool {
    (unsafe { sys::mrec_is_running() != 0 })
        || CAPTURE_CONTEXTS
            .lock()
            .map(|contexts| contexts.is_some())
            .unwrap_or(true)
}

fn meetings_watching() -> bool {
    unsafe { sys::mrec_is_watching() != 0 }
}

pub mod meetings {
    use super::{AudioProcess, Error, Meeting, MeetingEvent, Watcher};

    /// Every live process doing audio IO.
    pub fn audio_processes() -> Result<Vec<AudioProcess>, Error> {
        super::list_audio_processes()
    }

    /// Applications doing audio IO, grouped by owning PID, including helpers.
    /// Pass `pid` to capture the app; no meeting scan or Accessibility grant is needed.
    pub fn audio_apps() -> Result<Vec<AudioProcess>, Error> {
        super::list_audio_apps()
    }

    /// Point-in-time scan, highest confidence first.
    pub fn scan() -> Result<Vec<Meeting>, Error> {
        super::scan_meetings()
    }

    /// Watch for meetings starting, changing and ending.
    ///
    /// The handler runs on an internal serial queue, so it may allocate and
    /// block.
    pub fn watch<F>(handler: F) -> Result<Watcher, Error>
    where
        F: FnMut(MeetingEvent, &Meeting) + Send + 'static,
    {
        super::watch_meetings(handler)
    }

    pub fn watching() -> bool {
        super::meetings_watching()
    }
}

pub mod capture {
    use super::{CaptureOptions, CaptureSession, CaptureTarget, Error};

    /// Start capturing one process target or the system mix.
    ///
    /// Native initialization may block for six seconds. Cancellation during
    /// initialization is returned after that attempt has been cleaned up.
    pub fn start(target: CaptureTarget, options: CaptureOptions) -> Result<CaptureSession, Error> {
        super::start_capture(target, options)
    }

    pub fn running() -> bool {
        super::capture_running()
    }
}

#[cfg(test)]
mod capture_tests {
    use std::sync::atomic::AtomicUsize;

    use super::{PacketMeta, TrackQueue, STATE_PAUSED, STATE_RECORDING};

    fn write(
        queue: &TrackQueue,
        frames: &[f32],
        frame_count: u32,
        channels: u32,
        sample_rate: f64,
        host_time_ns: u64,
        state: &AtomicUsize,
    ) {
        queue.write(
            frames,
            PacketMeta {
                frame_count,
                channels,
                sample_rate,
                host_time_ns,
                segment: state.load(std::sync::atomic::Ordering::Acquire) >> 2,
                ..PacketMeta::default()
            },
            state,
        );
    }

    #[test]
    fn track_queue_drains_final_packets_while_producer_closes() {
        use std::{sync::Arc, thread};

        for _ in 0..32 {
            let queue = Arc::new(TrackQueue::new());
            let producer = Arc::clone(&queue);
            let writer = thread::spawn(move || {
                let state = AtomicUsize::new(STATE_RECORDING);
                for index in 0..128 {
                    write(&producer, &[0.25], 1, 1, 48_000.0, index * 20_833, &state);
                    thread::yield_now();
                }
                producer.close();
                write(&producer, &[1.0], 1, 1, 48_000.0, 3_000_000, &state);
            });
            let mut frames = Vec::new();
            while let Some(chunk) = queue.recv() {
                frames.extend(chunk.frames);
            }
            writer.join().unwrap();
            assert_eq!(frames, vec![0.25; 128]);
            assert!(queue.recv().is_none());
        }
    }

    #[test]
    fn track_queue_never_coalesces_across_format_changes() {
        let queue = TrackQueue::new();
        let state = AtomicUsize::new(STATE_RECORDING);
        write(&queue, &[1.0], 1, 1, 48_000.0, 1, &state);
        write(&queue, &[2.0, 3.0], 1, 2, 44_100.0, 2, &state);

        let first = queue.recv().unwrap();
        assert_eq!(first.frames, [1.0]);
        assert_eq!(first.sample_rate, 48_000.0);
        assert_eq!(first.channels, 1);

        let second = queue.recv().unwrap();
        assert_eq!(second.frames, [2.0, 3.0]);
        assert_eq!(second.frame_count, 1);
        assert_eq!(second.sample_rate, 44_100.0);
        assert_eq!(second.channels, 2);
        queue.close();
    }

    #[test]
    fn track_queue_never_coalesces_across_timestamp_gaps() {
        let queue = TrackQueue::new();
        let state = AtomicUsize::new(STATE_RECORDING);
        write(&queue, &[1.0], 1, 1, 48_000.0, 1_000_000, &state);
        write(&queue, &[2.0], 1, 1, 48_000.0, 20_000_000, &state);

        assert_eq!(queue.recv().unwrap().frames, [1.0]);
        assert_eq!(queue.recv().unwrap().frames, [2.0]);
        queue.close();
    }

    #[test]
    fn track_queue_reports_a_drop_on_the_first_packet_after_the_gap() {
        let queue = TrackQueue::new();
        let state = AtomicUsize::new(STATE_RECORDING);
        write(&queue, &[1.0], 1, 1, 48_000.0, 1_000_000, &state);
        let oversized = vec![0.0; super::TRACK_CAPACITY + 1];
        write(
            &queue,
            &oversized,
            oversized.len() as u32,
            1,
            48_000.0,
            2_000_000,
            &state,
        );
        write(&queue, &[2.0], 1, 1, 48_000.0, 3_000_000, &state);

        let before_gap = queue.recv().unwrap();
        assert_eq!(before_gap.frames, [1.0]);
        assert_eq!(before_gap.dropped_samples, 0);
        let after_gap = queue.recv().unwrap();
        assert_eq!(after_gap.frames, [2.0]);
        assert_eq!(after_gap.dropped_samples, oversized.len());
        assert_eq!(
            queue.dropped.load(std::sync::atomic::Ordering::Relaxed),
            oversized.len()
        );
        queue.close();
    }

    #[test]
    fn packets_keep_pause_boundaries_and_reject_stale_callbacks() {
        let queue = TrackQueue::new();
        let state = AtomicUsize::new(STATE_RECORDING);
        write(&queue, &[2.0], 1, 1, 48_000.0, 0, &state);
        queue.clear();
        write(&queue, &[0.25], 1, 1, 48_000.0, 1, &state);
        state.store(STATE_PAUSED, std::sync::atomic::Ordering::Release);
        write(&queue, &[1.0], 1, 1, 48_000.0, 2, &state);
        state.store(4, std::sync::atomic::Ordering::Release);
        write(&queue, &[0.5], 1, 1, 48_000.0, 3, &state);
        queue.write(
            &[1.0],
            PacketMeta {
                frame_count: 1,
                channels: 1,
                sample_rate: 48_000.0,
                host_time_ns: 4,
                segment: 0,
                ..PacketMeta::default()
            },
            &state,
        );
        queue.close();
        let first = queue.try_recv().unwrap();
        let second = queue.try_recv().unwrap();
        assert_eq!((first.segment, first.audio.frames), (0, vec![0.25]));
        assert_eq!((second.segment, second.audio.frames), (1, vec![0.5]));
        assert!(queue.try_recv().is_none());
    }

    #[test]
    fn native_callback_fans_out_independent_raw_and_mix_queues() {
        use super::{audio_trampoline, TrackCallbackContext};
        use std::sync::Arc;
        let state = Arc::new(AtomicUsize::new(STATE_RECORDING));
        let raw = Arc::new(TrackQueue::new());
        let mixed = Arc::new(TrackQueue::new());
        let context = TrackCallbackContext {
            queue: Arc::clone(&raw),
            mix_queue: Some(Arc::clone(&mixed)),
            state,
        };
        let samples = [0.25, -0.5];
        unsafe {
            audio_trampoline(
                samples.as_ptr(),
                2,
                1,
                48_000.0,
                42,
                (&context as *const TrackCallbackContext).cast_mut().cast(),
            );
        }
        assert_eq!(raw.try_recv().unwrap().audio.frames, samples);
        assert_eq!(mixed.try_recv().unwrap().audio.frames, samples);
        assert!(raw.try_recv().is_none());
        assert!(mixed.try_recv().is_none());
    }

    #[test]
    fn stopping_retains_native_flush_callbacks_but_paused_capture_does_not() {
        use super::{audio_trampoline, CaptureControl, EventQueue, TrackCallbackContext};
        use std::sync::{Arc, Mutex};
        for phase in [STATE_RECORDING, STATE_PAUSED] {
            let state = Arc::new(AtomicUsize::new(phase));
            let queue = Arc::new(TrackQueue::new());
            let context = TrackCallbackContext {
                queue: queue.clone(),
                mix_queue: None,
                state: state.clone(),
            };
            let control = CaptureControl {
                state,
                queues: vec![queue.clone()],
                raw_queues: vec![queue.clone()],
                events: Arc::new(EventQueue::default()),
                lifecycle: Mutex::new(()),
            };
            control.stop_with(|| unsafe {
                audio_trampoline(
                    [0.25f32, -0.5].as_ptr(),
                    2,
                    1,
                    48000.0,
                    42,
                    (&context as *const TrackCallbackContext).cast_mut().cast(),
                );
            });
            let packet = queue.recv();
            if phase == STATE_RECORDING {
                assert_eq!(packet.unwrap().frames, [0.25, -0.5]);
            } else {
                assert!(packet.is_none());
            }
            assert!(queue.recv().is_none());
        }
    }

    #[test]
    fn cancelled_requests_do_not_start_native_work() {
        use super::{
            capture, permissions, CancellationToken, CaptureOptions, CaptureTarget, Error,
            Permission,
        };
        use std::time::Duration;
        let token = CancellationToken::new();
        token.cancel();
        assert!(matches!(
            capture::start(
                CaptureTarget::System,
                CaptureOptions {
                    cancellation: Some(token.clone()),
                    ..CaptureOptions::default()
                }
            ),
            Err(Error::Cancelled)
        ));
        assert_eq!(
            permissions::request_and_wait(
                Permission::Microphone,
                Duration::from_secs(10),
                Some(&token)
            ),
            Err(Error::Cancelled)
        );
    }

    #[test]
    fn zero_mix_rate_is_rejected_before_capture_starts() {
        use super::{capture, CaptureOptions, CaptureTarget, Error, MixedAudioOptions};
        assert!(matches!(
            capture::start(
                CaptureTarget::System,
                CaptureOptions {
                    mix: Some(MixedAudioOptions { sample_rate: 0 }),
                    ..CaptureOptions::default()
                }
            ),
            Err(Error::Capture(_))
        ));
    }

    #[test]
    fn cancellation_wakes_permission_wait_without_polling_delay() {
        use super::{CancellationToken, Error};
        use std::{sync::mpsc, thread, time::Duration};
        let token = CancellationToken::new();
        let waiting = token.clone();
        let (send, recv) = mpsc::channel();
        let worker = thread::spawn(move || {
            send.send(waiting.wait(Duration::from_secs(60))).unwrap();
        });
        token.cancel();
        assert_eq!(
            recv.recv_timeout(Duration::from_secs(1)).unwrap(),
            Err(Error::Cancelled)
        );
        worker.join().unwrap();
    }

    #[test]
    fn cancellation_stops_registered_capture_and_preserves_tail() {
        use super::{CancellationToken, CaptureControl, STATE_STOPPED};
        use std::sync::{atomic::Ordering, Arc, Mutex};
        let queue = Arc::new(TrackQueue::new());
        let state = Arc::new(AtomicUsize::new(STATE_RECORDING));
        write(&queue, &[0.25], 1, 1, 48_000.0, 42, &state);
        let control = Arc::new(CaptureControl {
            state: Arc::clone(&state),
            queues: vec![Arc::clone(&queue)],
            raw_queues: vec![Arc::clone(&queue)],
            events: Arc::default(),
            lifecycle: Mutex::new(()),
        });
        let token = CancellationToken::new();
        token.register(&control).unwrap();
        token.cancel();
        assert_eq!(state.load(Ordering::Acquire) & 3, STATE_STOPPED);
        assert!(queue.closed.load(Ordering::Acquire));
        assert_eq!(queue.recv().unwrap().frames, [0.25]);
        assert!(queue.recv().is_none());
        control.stop();
    }
}
