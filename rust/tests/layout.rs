//! Guards the hand-written FFI structs in `sys` against the C headers.
//!
//! The C side is checked by `../tests/layout_check.c`; this is the Rust half. If
//! either drifts, memory is silently misread across the boundary.

use meeting_record::sys;

#[test]
fn meeting_layout_matches_header() {
    assert_eq!(std::mem::size_of::<sys::Meeting>(), 1768, "sizeof(mrec_meeting)");
    assert_eq!(std::mem::align_of::<sys::Meeting>(), 8);

    let m = unsafe { std::mem::zeroed::<sys::Meeting>() };
    let base = &m as *const _ as usize;
    let offset = |field: *const _| field as usize - base;

    assert_eq!(offset(&m.platform as *const _ as *const u8), 0);
    assert_eq!(offset(&m.pid as *const _ as *const u8), 4);
    assert_eq!(offset(&m.audio_pids as *const _ as *const u8), 8);
    assert_eq!(offset(&m.audio_pid_count as *const _ as *const u8), 72);
    assert_eq!(offset(&m.app_name as *const _ as *const u8), 80);
    assert_eq!(offset(&m.title as *const _ as *const u8), 208);
    assert_eq!(offset(&m.url as *const _ as *const u8), 720);
    assert_eq!(offset(&m.is_using_mic as *const _ as *const u8), 1744);
    assert_eq!(offset(&m.is_playing_audio as *const _ as *const u8), 1748);
    assert_eq!(offset(&m.confidence as *const _ as *const u8), 1752);
    assert_eq!(offset(&m.should_record as *const _ as *const u8), 1756);
    assert_eq!(offset(&m.detected_at_ns as *const _ as *const u8), 1760);
}

#[test]
fn process_layout_matches_header() {
    assert_eq!(std::mem::size_of::<sys::Process>(), 524);
    let p = unsafe { std::mem::zeroed::<sys::Process>() };
    let base = &p as *const _ as usize;
    assert_eq!(&p.pid as *const _ as usize - base, 0);
    assert_eq!(&p.is_running_output as *const _ as usize - base, 4);
    assert_eq!(&p.is_running_input as *const _ as usize - base, 8);
    assert_eq!(&p.bundle_id as *const _ as usize - base, 12);
    assert_eq!(&p.name as *const _ as usize - base, 268);
}

/// Exercises the real library: these must not crash or hang.
#[test]
fn reads_live_state() {
    let _ = meeting_record::audio_permission();
    let _ = meeting_record::accessibility_permission();
    let processes = meeting_record::audio_processes();
    let meetings = meeting_record::scan();
    assert!(!meeting_record::is_capturing());
    assert!(!meeting_record::is_watching());
    println!("{} audio processes, {} meetings", processes.len(), meetings.len());
}
