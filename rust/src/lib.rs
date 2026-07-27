//! System-audio and microphone capture with meeting detection for macOS and Windows.
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

mod sys;

use std::cell::UnsafeCell;
use std::error::Error as StdError;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::fmt::{Display, Formatter, Result as FmtResult};
use std::mem;
use std::panic::{self, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::sync::{
    atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering},
    Arc, Condvar, Mutex,
};
use std::thread;
use std::time::Duration;

use sys::{AudioCallback as RawAudioCallback, Meeting as RawMeeting, Process as RawProcess};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    UnsupportedOs,
    Permission(String),
    AlreadyRunning,
    Capture(String),
    Internal(String),
}

impl Display for Error {
    fn fmt(&self, f: &mut Formatter<'_>) -> FmtResult {
        match self {
            Error::UnsupportedOs => f.write_str("unsupported OS version"),
            Error::Permission(message) => write!(f, "permission not granted: {message}"),
            Error::AlreadyRunning => f.write_str("already running"),
            Error::Capture(message) => write!(f, "capture failed: {message}"),
            Error::Internal(message) => write!(f, "internal error: {message}"),
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
    if detail.is_empty() {
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Permission {
    SystemAudio,
    Microphone,
    Accessibility,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PermissionStatus {
    Unknown,
    Granted,
    Denied,
    /// The platform does not require an explicit prompt for this permission.
    NotRequired,
}

impl From<i32> for PermissionStatus {
    fn from(value: i32) -> Self {
        match value {
            1 => PermissionStatus::Granted,
            2 => PermissionStatus::Denied,
            3 => PermissionStatus::NotRequired,
            _ => PermissionStatus::Unknown,
        }
    }
}

pub mod permissions {
    use super::sys;
    use super::{Error, Permission, PermissionStatus};

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
    /// Accessibility opens System Settings and takes effect after an app relaunch.
    pub fn request(permission: Permission) -> Result<(), Error> {
        let status = match permission {
            Permission::SystemAudio => unsafe { sys::mrec_request_audio_permission() },
            Permission::Microphone => unsafe { sys::mrec_request_microphone_permission() },
            Permission::Accessibility => unsafe { sys::mrec_request_accessibility_permission() },
        };
        super::check(status)
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
fn list_audio_processes() -> Vec<AudioProcess> {
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
    unsafe { sys::mrec_list_audio_processes(buffer.as_mut_ptr(), buffer.len(), &mut count) };

    buffer.iter().take(count).map(AudioProcess::from).collect()
}

/// Conferencing platform.
///
/// [`Platform::as_str`] is a stable identifier. No human-readable labels are
/// provided; presentation and localisation belong to the caller.
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
    /// Requires the accessibility permission; empty otherwise.
    pub title: String,
    /// Requires the accessibility permission; empty otherwise.
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
fn scan_meetings() -> Vec<Meeting> {
    let mut buffer = [unsafe { mem::zeroed::<RawMeeting>() }; 16];
    let mut count = 0usize;
    unsafe { sys::mrec_scan(buffer.as_mut_ptr(), buffer.len(), &mut count) };
    buffer.iter().take(count).map(Meeting::from).collect()
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

// The watcher is a process-wide singleton, so the handler lives here rather than
// in the C API's `void*`.
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
    Default,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CaptureOptions {
    /// Mono mixdown for system audio. Defaults to true.
    pub mono: bool,
    /// Optional microphone track. Defaults to none.
    pub microphone: Option<MicrophoneSource>,
}

impl Default for CaptureOptions {
    fn default() -> Self {
        Self {
            mono: true,
            microphone: None,
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

/// Runtime health of the native audio devices, independent of pause/resume.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureHealth {
    Stopped,
    Running,
    /// A device or format changed and the backend is retrying automatically.
    Recovering,
    /// Automatic recovery was exhausted. Stop or drop this session before retrying.
    Failed,
}

fn capture_health() -> CaptureHealth {
    match unsafe { sys::mrec_capture_health_status() } {
        sys::MREC_CAPTURE_RUNNING => CaptureHealth::Running,
        sys::MREC_CAPTURE_RECOVERING => CaptureHealth::Recovering,
        sys::MREC_CAPTURE_FAILED => CaptureHealth::Failed,
        _ => CaptureHealth::Stopped,
    }
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
const STATE_RECORDING: u8 = 0;
const STATE_PAUSED: u8 = 1;
const STATE_STOPPED: u8 = 2;

#[derive(Clone, Copy, Default)]
struct PacketMeta {
    sample_start: usize,
    sample_count: usize,
    frame_count: u32,
    channels: u32,
    sample_rate: f64,
    host_time_ns: u64,
    dropped_before: usize,
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

    fn write(
        &self,
        frames: &[f32],
        frame_count: u32,
        channels: u32,
        sample_rate: f64,
        host_time_ns: u64,
        state: &AtomicU8,
    ) {
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

        if state.load(Ordering::Acquire) != STATE_RECORDING {
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
            unsafe {
                *self.packets[packet_head & self.packet_mask].get() = PacketMeta {
                    sample_start: head,
                    sample_count: frames.len(),
                    frame_count,
                    channels,
                    sample_rate,
                    host_time_ns,
                    dropped_before: self.dropped_since_read.swap(0, Ordering::Relaxed),
                };
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

    fn recv(&self) -> Option<AudioChunk> {
        loop {
            {
                let _read = self
                    .read_lock
                    .lock()
                    .unwrap_or_else(|error| error.into_inner());
                let packet_tail = self.packet_tail.load(Ordering::Relaxed);
                let packet_head = self.packet_head.load(Ordering::Acquire);
                if packet_tail != packet_head {
                    let first = unsafe { *self.packets[packet_tail & self.packet_mask].get() };
                    let mut next_packet = packet_tail;
                    let mut sample_count = 0usize;
                    let mut frame_count = 0u32;
                    while next_packet != packet_head {
                        let packet = unsafe { *self.packets[next_packet & self.packet_mask].get() };
                        if packet.channels != first.channels
                            || packet.sample_rate != first.sample_rate
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
                        frames.push(unsafe {
                            *self.buffer[(first.sample_start + offset) & self.mask].get()
                        });
                    }
                    self.tail.store(
                        first.sample_start.wrapping_add(sample_count),
                        Ordering::Release,
                    );
                    self.packet_tail.store(next_packet, Ordering::Release);
                    return Some(AudioChunk {
                        frames,
                        host_time_ns: first.host_time_ns,
                        frame_count,
                        channels: first.channels,
                        sample_rate: first.sample_rate,
                        dropped_samples: first.dropped_before,
                    });
                }
            }
            if self.closed.load(Ordering::Acquire) {
                return None;
            }
            if capture_health() == CaptureHealth::Failed {
                self.close();
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

    fn clear(&self) {
        while self
            .writing
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            thread::yield_now();
        }
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
        self.closed.store(true, Ordering::Release);
        self.wake.notify_all();
    }
}

/// One interleaved float32 PCM track.
pub struct AudioTrack {
    sample_rate: f64,
    channels: u32,
    queue: Arc<TrackQueue>,
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

    /// Number of interleaved samples discarded because the bounded queue could not accept them.
    pub fn dropped_samples(&self) -> usize {
        self.queue.dropped.load(Ordering::Relaxed)
    }

    /// Wait for the next chunk, or return `None` after the recording stops.
    pub fn recv(&self) -> Option<AudioChunk> {
        self.queue.recv()
    }
}

struct TrackCallbackContext {
    queue: Arc<TrackQueue>,
    state: Arc<AtomicU8>,
}

struct CaptureCallbackContexts {
    system_audio: Box<TrackCallbackContext>,
    microphone: Option<Box<TrackCallbackContext>>,
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
    if context.state.load(Ordering::Acquire) != STATE_RECORDING {
        return;
    }
    let len = frame_count as usize * channels.max(1) as usize;
    context.queue.write(
        slice::from_raw_parts(frames, len),
        frame_count,
        channels,
        sample_rate,
        host_time_ns,
        &context.state,
    );
}

/// Owns both audio tracks and their shared recording lifecycle.
#[must_use = "capture stops when this session is dropped"]
pub struct CaptureSession {
    system_audio: AudioTrack,
    microphone: Option<AudioTrack>,
    state: Arc<AtomicU8>,
    failure: Mutex<Option<String>>,
}

impl CaptureSession {
    pub fn system_audio(&self) -> &AudioTrack {
        &self.system_audio
    }

    pub fn microphone(&self) -> Option<&AudioTrack> {
        self.microphone.as_ref()
    }

    pub fn state(&self) -> CaptureState {
        if self.health() == CaptureHealth::Failed {
            return CaptureState::Stopped;
        }
        match self.state.load(Ordering::Acquire) {
            STATE_PAUSED => CaptureState::Paused,
            STATE_STOPPED => CaptureState::Stopped,
            _ => CaptureState::Recording,
        }
    }

    /// Native device health. Transient interruptions are recovered automatically.
    pub fn health(&self) -> CaptureHealth {
        if self.state.load(Ordering::Acquire) == STATE_STOPPED {
            CaptureHealth::Stopped
        } else {
            capture_health()
        }
    }

    /// The terminal recovery error, if automatic recovery was exhausted.
    pub fn failure(&self) -> Option<Error> {
        let mut failure = self
            .failure
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if failure.is_none() && self.health() == CaptureHealth::Failed {
            *failure = Some(Self::failure_detail());
        }
        failure.clone().map(Error::Capture)
    }

    /// Discard incoming frames until [`CaptureSession::resume`] is called.
    ///
    /// Paused time is omitted from the recording; silence is not synthesized.
    pub fn pause(&self) {
        if self
            .state
            .compare_exchange(
                STATE_RECORDING,
                STATE_PAUSED,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
        {
            self.system_audio.queue.clear();
            if let Some(microphone) = &self.microphone {
                microphone.queue.clear();
            }
        }
    }

    /// Resume delivering incoming frames.
    pub fn resume(&self) {
        if self.state.load(Ordering::Acquire) != STATE_PAUSED {
            return;
        }
        self.system_audio.queue.clear();
        if let Some(microphone) = &self.microphone {
            microphone.queue.clear();
        }
        let _ = self.state.compare_exchange(
            STATE_PAUSED,
            STATE_RECORDING,
            Ordering::AcqRel,
            Ordering::Acquire,
        );
    }

    /// Stop both tracks. Dropping the session has the same effect.
    pub fn stop(&self) {
        let terminal_failure =
            (capture_health() == CaptureHealth::Failed).then(Self::failure_detail);
        if self.state.swap(STATE_STOPPED, Ordering::AcqRel) == STATE_STOPPED {
            return;
        }
        if let Some(detail) = terminal_failure {
            *self
                .failure
                .lock()
                .unwrap_or_else(|error| error.into_inner()) = Some(detail);
        }
        unsafe { sys::mrec_stop() };
        self.system_audio.queue.close();
        if let Some(microphone) = &self.microphone {
            microphone.queue.close();
        }
        *CAPTURE_CONTEXTS
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = None;
    }

    fn failure_detail() -> String {
        let detail = last_error();
        if detail.is_empty() || detail == "no error" {
            "automatic capture recovery failed".to_owned()
        } else {
            detail
        }
    }
}

impl Drop for CaptureSession {
    fn drop(&mut self) {
        self.stop();
    }
}

fn resolve_process_target(pid: u32) -> Result<Vec<u32>, Error> {
    if pid == 0 {
        return Err(Error::Capture("process pid must be non-zero".to_owned()));
    }

    #[cfg(target_os = "windows")]
    {
        // WASAPI INCLUDE_PROCESS_TREE follows current and future helper children.
        // Adding active helpers separately would record them twice.
        return Ok(vec![pid]);
    }

    #[cfg(not(target_os = "windows"))]
    {
        let mut meetings = [unsafe { mem::zeroed::<RawMeeting>() }; 16];
        let mut count = 0usize;
        unsafe { sys::mrec_scan(meetings.as_mut_ptr(), meetings.len(), &mut count) };

        let mut resolved = meetings
            .iter()
            .take(count)
            .find(|meeting| meeting.pid == pid)
            .map(|meeting| {
                meeting.audio_pids[..meeting.audio_pid_count.min(sys::MREC_MAX_AUDIO_PIDS)].to_vec()
            })
            .unwrap_or_default();
        resolved.retain(|candidate| *candidate != 0);
        if resolved.is_empty() {
            resolved.push(pid);
        }
        Ok(resolved)
    }
}

/// Capture one process target or the system mix.
///
/// Blocks for up to six seconds while the permission grant is undetermined, so do
/// not call from a UI thread.
fn start_capture(target: CaptureTarget, options: CaptureOptions) -> Result<CaptureSession, Error> {
    if unsafe { sys::mrec_is_running() } != 0 {
        return Err(Error::AlreadyRunning);
    }

    let (pids, system_wide) = match target {
        CaptureTarget::Process { pid } => (resolve_process_target(pid)?, false),
        CaptureTarget::System => (Vec::new(), true),
    };

    let state = Arc::new(AtomicU8::new(STATE_RECORDING));
    let system_queue = Arc::new(TrackQueue::new());
    let microphone_queue = options.microphone.map(|_| Arc::new(TrackQueue::new()));
    let mut contexts = CaptureCallbackContexts {
        system_audio: Box::new(TrackCallbackContext {
            queue: Arc::clone(&system_queue),
            state: Arc::clone(&state),
        }),
        microphone: microphone_queue.as_ref().map(|queue| {
            Box::new(TrackCallbackContext {
                queue: Arc::clone(queue),
                state: Arc::clone(&state),
            })
        }),
    };
    let system_context = (&mut *contexts.system_audio) as *mut TrackCallbackContext as *mut c_void;
    let microphone_context = contexts
        .microphone
        .as_mut()
        .map(|context| (&mut **context) as *mut TrackCallbackContext as *mut c_void)
        .unwrap_or(ptr::null_mut());
    let mut context_slot = CAPTURE_CONTEXTS
        .lock()
        .map_err(|error| Error::Internal(error.to_string()))?;
    if context_slot.is_some() {
        return Err(Error::AlreadyRunning);
    }
    *context_slot = Some(contexts);
    drop(context_slot);

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
                None => 0,
            },
            Some(audio_trampoline),
            system_context,
            options
                .microphone
                .map(|_| audio_trampoline as RawAudioCallback),
            microphone_context,
        )
    };
    if status != sys::MREC_OK {
        *CAPTURE_CONTEXTS.lock().unwrap() = None;
        system_queue.close();
        if let Some(queue) = &microphone_queue {
            queue.close();
        }
        check(status)?;
    }

    if capture_health() == CaptureHealth::Failed {
        unsafe { sys::mrec_stop() };
        system_queue.close();
        if let Some(queue) = &microphone_queue {
            queue.close();
        }
        *CAPTURE_CONTEXTS.lock().unwrap() = None;
        return Err(Error::Capture(last_error()));
    }

    let mut system_sample_rate = 0.0f64;
    let mut system_channels = 0u32;
    let system_format_status =
        unsafe { sys::mrec_current_format(&mut system_sample_rate, &mut system_channels) };
    if let Err(error) = check(system_format_status) {
        unsafe { sys::mrec_stop() };
        system_queue.close();
        if let Some(queue) = &microphone_queue {
            queue.close();
        }
        *CAPTURE_CONTEXTS.lock().unwrap() = None;
        return Err(error);
    }
    let microphone = if let Some(queue) = microphone_queue {
        let mut sample_rate = 0.0f64;
        let mut channels = 0u32;
        let microphone_format_status =
            unsafe { sys::mrec_current_microphone_format(&mut sample_rate, &mut channels) };
        if let Err(error) = check(microphone_format_status) {
            unsafe { sys::mrec_stop() };
            system_queue.close();
            queue.close();
            *CAPTURE_CONTEXTS.lock().unwrap() = None;
            return Err(error);
        }
        Some(AudioTrack {
            sample_rate,
            channels,
            queue,
        })
    } else {
        None
    };

    Ok(CaptureSession {
        system_audio: AudioTrack {
            sample_rate: system_sample_rate,
            channels: system_channels,
            queue: system_queue,
        },
        microphone,
        state,
        failure: Mutex::new(None),
    })
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
    pub fn audio_processes() -> Vec<AudioProcess> {
        super::list_audio_processes()
    }

    /// Point-in-time scan, highest confidence first.
    pub fn scan() -> Vec<Meeting> {
        super::scan_meetings()
    }

    /// Watch for meetings starting, changing and ending.
    ///
    /// The handler runs on an internal serial queue, so it may allocate and block.
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
    pub fn start(target: CaptureTarget, options: CaptureOptions) -> Result<CaptureSession, Error> {
        super::start_capture(target, options)
    }

    pub fn running() -> bool {
        super::capture_running()
    }
}

#[cfg(test)]
mod capture_tests {
    use std::sync::atomic::AtomicU8;

    use super::{TrackQueue, STATE_PAUSED, STATE_RECORDING};

    #[test]
    fn track_queue_delivers_interleaved_samples() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        queue.write(&[0.25, -0.5, 0.75, -1.0], 2, 2, 48_000.0, 42, &state);
        let chunk = queue.recv().unwrap();
        assert_eq!(chunk.frames, [0.25, -0.5, 0.75, -1.0]);
        assert_eq!(chunk.frame_count, 2);
        assert_eq!(chunk.channels, 2);
        assert_eq!(chunk.sample_rate, 48_000.0);
        assert_eq!(chunk.host_time_ns, 42);
        assert_eq!(chunk.dropped_samples, 0);
        queue.close();
        assert!(queue.recv().is_none());
    }

    #[test]
    fn track_queue_drops_paused_samples() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_PAUSED);
        queue.write(&[1.0], 1, 1, 48_000.0, 1, &state);
        queue.close();
        assert!(queue.recv().is_none());
    }

    #[test]
    fn track_queue_clears_buffered_samples() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        queue.write(&[2.0], 1, 1, 48_000.0, 1, &state);
        queue.clear();
        queue.close();
        assert!(queue.recv().is_none());
    }

    #[test]
    fn track_queue_coalesces_matching_packets_and_keeps_first_timestamp() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        queue.write(&[1.0, 2.0], 2, 1, 48_000.0, 1_000, &state);
        queue.write(&[3.0, 4.0], 2, 1, 48_000.0, 1_042, &state);

        let chunk = queue.recv().unwrap();
        assert_eq!(chunk.frames, [1.0, 2.0, 3.0, 4.0]);
        assert_eq!(chunk.frame_count, 4);
        assert_eq!(chunk.host_time_ns, 1_000);
        queue.close();
    }

    #[test]
    fn track_queue_never_coalesces_across_format_changes() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        queue.write(&[1.0], 1, 1, 48_000.0, 1, &state);
        queue.write(&[2.0, 3.0], 1, 2, 44_100.0, 2, &state);

        let first = queue.recv().unwrap();
        assert_eq!(first.frames, [1.0]);
        assert_eq!(first.sample_rate, 48_000.0);
        assert_eq!(first.channels, 1);

        let second = queue.recv().unwrap();
        assert_eq!(second.frames, [2.0, 3.0]);
        assert_eq!(second.sample_rate, 44_100.0);
        assert_eq!(second.channels, 2);
        queue.close();
    }

    #[test]
    fn track_queue_never_coalesces_across_timestamp_gaps() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        queue.write(&[1.0], 1, 1, 48_000.0, 1_000_000, &state);
        queue.write(&[2.0], 1, 1, 48_000.0, 20_000_000, &state);

        assert_eq!(queue.recv().unwrap().frames, [1.0]);
        assert_eq!(queue.recv().unwrap().frames, [2.0]);
        queue.close();
    }

    #[test]
    fn track_queue_reports_a_drop_on_the_first_packet_after_the_gap() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        queue.write(&[1.0], 1, 1, 48_000.0, 1_000_000, &state);
        let oversized = vec![0.0; super::TRACK_CAPACITY + 1];
        queue.write(
            &oversized,
            oversized.len() as u32,
            1,
            48_000.0,
            2_000_000,
            &state,
        );
        queue.write(&[2.0], 1, 1, 48_000.0, 3_000_000, &state);

        let before_gap = queue.recv().unwrap();
        assert_eq!(before_gap.frames, [1.0]);
        assert_eq!(before_gap.dropped_samples, 0);
        let after_gap = queue.recv().unwrap();
        assert_eq!(after_gap.frames, [2.0]);
        assert_eq!(after_gap.dropped_samples, oversized.len());
        queue.close();
    }

    #[test]
    fn track_queue_reports_bounded_buffer_drops() {
        let queue = TrackQueue::new();
        let state = AtomicU8::new(STATE_RECORDING);
        let oversized = vec![0.0; super::TRACK_CAPACITY + 1];
        queue.write(&oversized, oversized.len() as u32, 1, 48_000.0, 1, &state);
        queue.write(&[1.0], 1, 1, 48_000.0, 2, &state);

        let chunk = queue.recv().unwrap();
        assert_eq!(chunk.frames, [1.0]);
        assert_eq!(chunk.dropped_samples, oversized.len());
        assert_eq!(
            queue.dropped.load(std::sync::atomic::Ordering::Relaxed),
            oversized.len()
        );
        queue.close();
    }
}
