//! Raw FFI declarations for the C ABI in `native/include/`.
//!
//! Hand-written to avoid a build-time dependency on libclang. Field order and
//! padding must match the headers; `tests/layout.rs` asserts it.

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

pub const MREC_CAPTURE_RUNNING: i32 = 1;
pub const MREC_CAPTURE_RECOVERING: i32 = 2;
pub const MREC_CAPTURE_FAILED: i32 = 3;

pub const MREC_MAX_AUDIO_PIDS: usize = 16;

/// Interleaved float32 PCM on a realtime audio thread.
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
    pub fn mrec_microphone_permission_status() -> c_int;
    pub fn mrec_request_microphone_permission() -> c_int;
    pub fn mrec_accessibility_permission_status() -> c_int;
    pub fn mrec_request_accessibility_permission() -> c_int;

    pub fn mrec_list_audio_processes(
        out: *mut Process,
        capacity: usize,
        out_count: *mut usize,
    ) -> c_int;

    pub fn mrec_start_tracks_raw(
        pids: *const u32,
        pid_count: usize,
        global_mixdown: i32,
        mono: i32,
        mute_captured_output: i32,
        microphone: i32,
        system_audio_cb: Option<AudioCallback>,
        system_audio_user_data: *mut c_void,
        microphone_cb: Option<AudioCallback>,
        microphone_user_data: *mut c_void,
    ) -> i32;

    pub fn mrec_stop() -> c_int;
    pub fn mrec_is_running() -> i32;
    pub fn mrec_capture_health_status() -> i32;
    pub fn mrec_current_format(sample_rate: *mut c_double, channels: *mut u32) -> c_int;
    pub fn mrec_current_microphone_format(sample_rate: *mut c_double, channels: *mut u32) -> c_int;
    pub fn mrec_last_error() -> *const c_char;

    pub fn mrec_scan(out: *mut Meeting, capacity: usize, out_count: *mut usize) -> c_int;
    pub fn mrec_watch_start(cb: Option<MeetingCallback>, user_data: *mut c_void) -> c_int;
    pub fn mrec_watch_stop() -> c_int;
    pub fn mrec_is_watching() -> i32;
}

#[cfg(test)]
mod tests {
    use std::mem;

    use super::{Meeting, Process};

    #[test]
    fn meeting_layout_matches_header() {
        assert_eq!(mem::size_of::<Meeting>(), 1768, "sizeof(mrec_meeting)");
        assert_eq!(mem::align_of::<Meeting>(), 8);

        let meeting = unsafe { mem::zeroed::<Meeting>() };
        let base = &meeting as *const _ as usize;
        let offset = |field: *const _| field as usize - base;

        assert_eq!(offset(&meeting.platform as *const _ as *const u8), 0);
        assert_eq!(offset(&meeting.pid as *const _ as *const u8), 4);
        assert_eq!(offset(&meeting.audio_pids as *const _ as *const u8), 8);
        assert_eq!(
            offset(&meeting.audio_pid_count as *const _ as *const u8),
            72
        );
        assert_eq!(offset(&meeting.app_name as *const _ as *const u8), 80);
        assert_eq!(offset(&meeting.title as *const _ as *const u8), 208);
        assert_eq!(offset(&meeting.url as *const _ as *const u8), 720);
        assert_eq!(offset(&meeting.is_using_mic as *const _ as *const u8), 1744);
        assert_eq!(
            offset(&meeting.is_playing_audio as *const _ as *const u8),
            1748
        );
        assert_eq!(offset(&meeting.confidence as *const _ as *const u8), 1752);
        assert_eq!(
            offset(&meeting.should_record as *const _ as *const u8),
            1756
        );
        assert_eq!(
            offset(&meeting.detected_at_ns as *const _ as *const u8),
            1760
        );
    }

    #[test]
    fn process_layout_matches_header() {
        assert_eq!(mem::size_of::<Process>(), 524);
        let process = unsafe { mem::zeroed::<Process>() };
        let base = &process as *const _ as usize;
        assert_eq!(&process.pid as *const _ as usize - base, 0);
        assert_eq!(&process.is_running_output as *const _ as usize - base, 4);
        assert_eq!(&process.is_running_input as *const _ as usize - base, 8);
        assert_eq!(&process.bundle_id as *const _ as usize - base, 12);
        assert_eq!(&process.name as *const _ as usize - base, 268);
    }
}
