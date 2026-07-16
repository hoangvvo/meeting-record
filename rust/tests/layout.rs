use meeting_record::{capture, meetings, permissions, Permission};

/// Exercises the real library.
#[test]
fn reads_live_state() {
    let _ = permissions::status(Permission::SystemAudio);
    let _ = permissions::status(Permission::Accessibility);
    let processes = meetings::audio_processes();
    let detected = meetings::scan();
    assert!(!capture::running());
    assert!(!meetings::watching());
    println!(
        "{} audio processes, {} meetings",
        processes.len(),
        detected.len()
    );
}
