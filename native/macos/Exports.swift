import CoreAudio
import Foundation

/// C ABI surface declared in native/include/meeting-record.h.
///
/// Everything is funnelled through one process-wide capture session, mirroring
/// the underlying reality: a single tap + aggregate device pair per process.

private final class Session {
    static let shared = Session()

    let lock = NSLock()
    let stateBox = CaptureStateBox()
    var capture: TapCapture?
    var lastError = "no error"
}

// MARK: - Errors

private var lastErrorStorage: UnsafeMutablePointer<CChar>?

private func setLastError(_ message: String) {
    Session.shared.lastError = message
    if let old = lastErrorStorage { free(old) }
    lastErrorStorage = strdup(message)
}

@_cdecl("mrec_last_error")
public func mrec_last_error() -> UnsafePointer<CChar>? {
    if lastErrorStorage == nil { lastErrorStorage = strdup(Session.shared.lastError) }
    return UnsafePointer(lastErrorStorage)
}

// MARK: - Permissions

@_cdecl("mrec_audio_permission_status")
public func mrec_audio_permission_status() -> Int32 {
    Permissions.audioCaptureStatus()
}

@_cdecl("mrec_request_audio_permission")
public func mrec_request_audio_permission() -> Int32 {
    Permissions.requestAudioCapture()
}

// MARK: - Process enumeration

private let bundleIDOffset = 12
private let nameOffset = 12 + 256
private let processStride = 12 + 256 + 256

@_cdecl("mrec_list_audio_processes")
public func mrec_list_audio_processes(_ out: UnsafeMutableRawPointer?,
                                        _ capacity: Int,
                                        _ outCount: UnsafeMutablePointer<Int>?) -> Int32 {
    guard let out, let outCount else { return -9 /* INTERNAL */ }

    let processes = AudioProcessRegistry.all()
    let count = min(processes.count, capacity)

    for index in 0..<count {
        let process = processes[index]
        let base = out.advanced(by: index * processStride)
        base.assumingMemoryBound(to: UInt32.self).pointee = UInt32(process.pid)
        base.advanced(by: 4).assumingMemoryBound(to: Int32.self).pointee =
            process.isRunningOutput ? 1 : 0
        base.advanced(by: 8).assumingMemoryBound(to: Int32.self).pointee =
            process.isRunningInput ? 1 : 0
        writeCString(process.bundleID, to: base.advanced(by: bundleIDOffset), capacity: 256)
        writeCString(process.name, to: base.advanced(by: nameOffset), capacity: 256)
    }

    outCount.pointee = count
    return processes.count > capacity ? -10 /* BUFFER_TOO_SMALL */ : 0
}

private func writeCString(_ value: String, to pointer: UnsafeMutableRawPointer, capacity: Int) {
    let bytes = Array(value.utf8.prefix(capacity - 1))
    let dest = pointer.assumingMemoryBound(to: UInt8.self)
    dest.update(repeating: 0, count: capacity)
    if !bytes.isEmpty { dest.update(from: bytes, count: bytes.count) }
}

// MARK: - Capture

/// Flat entry point. `@_cdecl` cannot accept a pointer to a Swift struct, so the
/// ergonomic `mrec_config` form lives in meeting-record.h as a static inline wrapper
/// around this function.
@_cdecl("mrec_start_raw")
public func mrec_start_raw(_ pidsPtr: UnsafePointer<UInt32>?,
                             _ pidCount: Int,
                             _ globalMixdown: Int32,
                             _ mono: Int32,
                             _ muteCapturedOutput: Int32,
                             _ callback: MrecAudioCallback?,
                             _ userData: UnsafeMutableRawPointer?) -> Int32 {
    guard #available(macOS 14.2, *) else {
        setLastError("CoreAudio process taps require macOS 14.2 or newer")
        return -1
    }

    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    if session.capture != nil {
        setLastError("capture already running")
        return -3
    }

    var pids: [pid_t] = []
    if let pidsPtr, pidCount > 0 {
        for index in 0..<pidCount { pids.append(pid_t(pidsPtr[index])) }
    }
    let global = globalMixdown != 0

    // With neither explicit pids nor an explicit global request, default to every
    // process currently rendering audio — that keeps capture working while the
    // user's output is muted, which a global tap would not.
    if pids.isEmpty && !global {
        pids = AudioProcessRegistry.activeOutput().map(\.pid)
        if pids.isEmpty {
            setLastError("no process is currently playing audio; "
                + "pass explicit pids or set global_mixdown")
            return -5
        }
    }

    session.stateBox.configure(callback: callback, userData: userData)
    let capture = TapCapture(stateBox: session.stateBox)

    // Attempt the real start under a timeout rather than probing permission
    // first. Probing means building and tearing down a second tap moments before
    // this one, which is both slow and unreliable — the teardown races the new
    // tap's auto-start and can yield a running-but-silent stream. One attempt,
    // time-bounded, is cheaper and more accurate.
    //
    // A timeout here almost always means the TCC grant is still undetermined:
    // that is exactly the case where CoreAudio blocks instead of failing.
    let outcome: Int32? = withTimeout(seconds: 6) {
        do {
            try capture.start(pids: pids,
                              globalMixdown: global,
                              mono: mono != 0,
                              muteCapturedOutput: muteCapturedOutput != 0)
            return 0
        } catch let error as CaptureError {
            setLastError(error.message)
            return error.status
        } catch {
            setLastError("\(error)")
            return -9
        }
    }

    guard let outcome else {
        session.stateBox.clear()
        setLastError("timed out starting capture; system audio recording "
            + "permission is most likely not granted yet — call "
            + "mrec_request_audio_permission() from a GUI process, or grant it "
            + "under System Settings > Privacy & Security > "
            + "Screen & System Audio Recording")
        return -2
    }
    guard outcome == 0 else {
        session.stateBox.clear()
        return outcome
    }

    session.capture = capture
    setLastError("no error")
    return 0
}

@_cdecl("mrec_stop")
public func mrec_stop() -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard let capture = session.capture else { return -4 /* NOT_RUNNING */ }
    capture.stop()
    session.capture = nil
    session.stateBox.clear()
    return 0
}

@_cdecl("mrec_is_running")
public func mrec_is_running() -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }
    return session.capture != nil ? 1 : 0
}

@_cdecl("mrec_current_format")
public func mrec_current_format(_ sampleRate: UnsafeMutablePointer<Double>?,
                                  _ channels: UnsafeMutablePointer<UInt32>?) -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard session.capture != nil else { return -4 }
    let format = session.stateBox.currentFormat()
    sampleRate?.pointee = format.sampleRate
    channels?.pointee = format.channels
    return 0
}
