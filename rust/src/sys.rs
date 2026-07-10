//! Raw FFI declarations for the C ABI in `native/include/`.
//!
//! Hand-written rather than bindgen-generated: the surface is 16 functions and two
//! structs, and hand-writing keeps the crate free of a build-time dependency on
//! libclang.
//!
//! Field order and padding here must match the headers exactly. `layout_check.c`
//! guards the C side; `tests/layout.rs` guards this side.

use std::os::raw::{c_char, c_double, c_int, c_void};

pub const MREC_OK: i32 = 0;
pub const MREC_ERR_UNSUPPORTED_OS: i32 = -1;
pub const MREC_ERR_PERMISSION: i32 = -2;
pub const MREC_ERR_ALREADY_RUNNING: i32 = -3;
pub const MREC_ERR_NOT_RUNNING: i32 = -4;
pub const MREC_ERR_NO_PROCESSES: i32 = -5;
pub const MREC_ERR_TAP_FAILED: i32 = -6;
pub const MREC_ERR_DEVICE_FAILED: i32 = -7;
pub const MREC_ERR_IOPROC_FAILED: i32 = -8;
pub const MREC_ERR_INTERNAL: i32 = -9;
pub const MREC_ERR_BUFFER_TOO_SMALL: i32 = -10;

pub const MREC_MAX_AUDIO_PIDS: usize = 16;

/// Interleaved float32 PCM, delivered on a realtime audio thread.
pub type AudioCallback = unsafe extern "C" fn(
    frames: *const f32,
    frame_count: u32,
    channels: u32,
    sample_rate: c_double,
    host_time_ns: u64,
    user_data: *mut c_void,
);

pub type MeetingCallback =
    unsafe extern "C" fn(meeting: *const Meeting, event: c_int, user_data: *mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Process {
    pub pid: u32,
    pub is_running_output: i32,
    pub is_running_input: i32,
    pub bundle_id: [c_char; 256],
    pub name: [c_char; 256],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Config {
    pub pids: *const u32,
    pub pid_count: usize,
    pub global_mixdown: i32,
    pub mono: i32,
    pub mute_captured_output: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Meeting {
    pub platform: i32,
    pub pid: u32,
    pub audio_pids: [u32; MREC_MAX_AUDIO_PIDS],
    pub audio_pid_count: usize,
    pub app_name: [c_char; 128],
    pub title: [c_char; 512],
    pub url: [c_char; 1024],
    pub is_using_mic: i32,
    pub is_playing_audio: i32,
    pub confidence: i32,
    pub should_record: i32,
    pub detected_at_ns: u64,
}

extern "C" {
    pub fn mrec_audio_permission_status() -> c_int;
    pub fn mrec_request_audio_permission() -> c_int;
    pub fn mrec_accessibility_permission_status() -> c_int;
    pub fn mrec_request_accessibility_permission() -> c_int;

    pub fn mrec_list_audio_processes(
        out: *mut Process,
        capacity: usize,
        out_count: *mut usize,
    ) -> c_int;

    /// `mrec_start` is `static inline` in the header, so only this is a real symbol.
    pub fn mrec_start_raw(
        pids: *const u32,
        pid_count: usize,
        global_mixdown: i32,
        mono: i32,
        mute_captured_output: i32,
        cb: Option<AudioCallback>,
        user_data: *mut c_void,
    ) -> i32;

    pub fn mrec_start_meeting(
        meeting: *const Meeting,
        cb: Option<AudioCallback>,
        user_data: *mut c_void,
    ) -> c_int;

    pub fn mrec_stop() -> c_int;
    pub fn mrec_is_running() -> i32;
    pub fn mrec_current_format(sample_rate: *mut c_double, channels: *mut u32) -> c_int;
    pub fn mrec_last_error() -> *const c_char;

    pub fn mrec_scan(out: *mut Meeting, capacity: usize, out_count: *mut usize) -> c_int;
    pub fn mrec_watch_start(cb: Option<MeetingCallback>, user_data: *mut c_void) -> c_int;
    pub fn mrec_watch_stop() -> c_int;
    pub fn mrec_is_watching() -> i32;
    pub fn mrec_platform_name(platform: i32) -> *const c_char;
}
