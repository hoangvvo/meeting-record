import CoreAudio
import Foundation

/// C ABI declared in native/include/meeting-record.h.
///
/// One process-wide capture session, matching the one tap and aggregate device
/// pair a process can hold.

private final class Session {
    static let shared = Session()
    static let errorCapacity = 1024

    let lock = NSLock()
    let systemStateBox = CaptureStateBox()
    let microphoneStateBox = CaptureStateBox()
    var capture: TapCapture?
    var microphone: MicrophoneCapture?
    var captureConfiguration: SystemCaptureConfiguration?
    let restartQueue = DispatchQueue(label: "meetingrecord.capture-restart")
    let healthLock = NSLock()
    var health: Int32 = 0
    let errorLock = NSLock()
    let lastErrorStorage: UnsafeMutablePointer<CChar>

    private init() {
        lastErrorStorage = .allocate(capacity: Self.errorCapacity)
        lastErrorStorage.initialize(repeating: 0, count: Self.errorCapacity)
        let initial = Array("no error".utf8)
        for (index, byte) in initial.enumerated() {
            lastErrorStorage[index] = CChar(bitPattern: byte)
        }
    }
}

private func setCaptureHealth(_ value: Int32) {
    let session = Session.shared
    session.healthLock.lock()
    session.health = value
    session.healthLock.unlock()
}

private func captureHealth() -> Int32 {
    let session = Session.shared
    session.healthLock.lock()
    let value = session.health
    session.healthLock.unlock()
    return value
}

private struct SystemCaptureConfiguration {
    var pids: [pid_t]
    var globalMixdown: Bool
    var mono: Bool
    var muteCapturedOutput: Bool
}

private func setLastError(_ message: String) {
    let session = Session.shared
    session.errorLock.lock()
    session.lastErrorStorage.update(repeating: 0, count: Session.errorCapacity)
    for (index, byte) in message.utf8.prefix(Session.errorCapacity - 1).enumerated() {
        session.lastErrorStorage[index] = CChar(bitPattern: byte)
    }
    session.errorLock.unlock()
}

private func makeTapCapture(_ configuration: SystemCaptureConfiguration) throws -> TapCapture {
    let capture = TapCapture(stateBox: Session.shared.systemStateBox,
                             onConfigurationChange: scheduleCaptureRestart)
    try capture.start(pids: configuration.pids,
                      globalMixdown: configuration.globalMixdown,
                      mono: configuration.mono,
                      muteCapturedOutput: configuration.muteCapturedOutput)
    return capture
}

private func makeMicrophoneCapture() throws -> MicrophoneCapture {
    let microphone = MicrophoneCapture(stateBox: Session.shared.microphoneStateBox,
                                       onConfigurationChange: scheduleMicrophoneRestart)
    try microphone.start()
    return microphone
}

private func scheduleCaptureRestart(_ identifier: UUID, _ reason: String) {
    let session = Session.shared
    session.restartQueue.async {
        session.lock.lock()
        defer { session.lock.unlock() }
        guard let current = session.capture,
              current.identifier == identifier,
              let configuration = session.captureConfiguration else { return }

        setCaptureHealth(2) // MREC_CAPTURE_RECOVERING
        setLastError("system audio interrupted: \(reason); restarting")
        current.stop()
        session.capture = nil

        var lastFailure = "unknown error"
        for attempt in 0..<3 {
            if attempt > 0 {
                Thread.sleep(forTimeInterval: 0.1 * Double(attempt + 1))
            }
            do {
                session.capture = try makeTapCapture(configuration)
                setLastError("no error")
                setCaptureHealth(1) // MREC_CAPTURE_RUNNING
                return
            } catch let error as CaptureError {
                lastFailure = error.message
            } catch {
                lastFailure = String(describing: error)
            }
        }

        session.microphone?.stop()
        session.microphone = nil
        session.captureConfiguration = nil
        session.systemStateBox.clear()
        session.microphoneStateBox.clear()
        setLastError("system audio recovery failed after 3 attempts: \(lastFailure)")
        setCaptureHealth(3) // MREC_CAPTURE_FAILED
    }
}

private func scheduleMicrophoneRestart(_ identifier: UUID, _ reason: String) {
    let session = Session.shared
    session.restartQueue.async {
        session.lock.lock()
        defer { session.lock.unlock() }
        guard let current = session.microphone,
              current.identifier == identifier,
              session.capture != nil else { return }

        setCaptureHealth(2) // MREC_CAPTURE_RECOVERING
        setLastError("microphone interrupted: \(reason); restarting")
        current.stop()
        session.microphone = nil

        var lastFailure = "unknown error"
        for attempt in 0..<3 {
            if attempt > 0 {
                Thread.sleep(forTimeInterval: 0.1 * Double(attempt + 1))
            }
            do {
                session.microphone = try makeMicrophoneCapture()
                setLastError("no error")
                setCaptureHealth(1) // MREC_CAPTURE_RUNNING
                return
            } catch let error as CaptureError {
                lastFailure = error.message
            } catch {
                lastFailure = String(describing: error)
            }
        }

        session.capture?.stop()
        session.capture = nil
        session.captureConfiguration = nil
        session.systemStateBox.clear()
        session.microphoneStateBox.clear()
        setLastError("microphone recovery failed after 3 attempts: \(lastFailure)")
        setCaptureHealth(3) // MREC_CAPTURE_FAILED
    }
}

@_cdecl("mrec_last_error")
public func mrec_last_error() -> UnsafePointer<CChar>? {
    let session = Session.shared
    let key = "dev.meetingrecord.last-error"
    let dictionary = Thread.current.threadDictionary
    let snapshot: NSMutableData
    if let existing = dictionary[key] as? NSMutableData,
       existing.length == Session.errorCapacity {
        snapshot = existing
    } else {
        snapshot = NSMutableData(length: Session.errorCapacity)!
        dictionary[key] = snapshot
    }

    session.errorLock.lock()
    snapshot.mutableBytes.copyMemory(from: session.lastErrorStorage,
                                     byteCount: Session.errorCapacity)
    session.errorLock.unlock()
    return UnsafePointer(snapshot.mutableBytes.assumingMemoryBound(to: CChar.self))
}

@_cdecl("mrec_audio_permission_status")
public func mrec_audio_permission_status() -> Int32 {
    Permissions.audioCaptureStatus()
}

@_cdecl("mrec_request_audio_permission")
public func mrec_request_audio_permission() -> Int32 {
    Permissions.requestAudioCapture()
}

@_cdecl("mrec_microphone_permission_status")
public func mrec_microphone_permission_status() -> Int32 {
    Permissions.microphoneStatus()
}

@_cdecl("mrec_request_microphone_permission")
public func mrec_request_microphone_permission() -> Int32 {
    Permissions.requestMicrophone()
}

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

/// `@_cdecl` cannot accept a pointer to a Swift struct, so `mrec_config` lives in
/// meeting-record.h as a static inline wrapper around this.
@_cdecl("mrec_start_raw")
public func mrec_start_raw(_ pidsPtr: UnsafePointer<UInt32>?,
                             _ pidCount: Int,
                             _ globalMixdown: Int32,
                             _ mono: Int32,
                             _ muteCapturedOutput: Int32,
                             _ callback: MrecAudioCallback?,
                             _ userData: UnsafeMutableRawPointer?) -> Int32 {
    mrec_start_tracks_raw(pidsPtr, pidCount, globalMixdown, mono,
                          muteCapturedOutput, 0, callback, userData, nil, nil)
}

@_cdecl("mrec_start_tracks_raw")
public func mrec_start_tracks_raw(_ pidsPtr: UnsafePointer<UInt32>?,
                                    _ pidCount: Int,
                                    _ globalMixdown: Int32,
                                    _ mono: Int32,
                                    _ muteCapturedOutput: Int32,
                                    _ microphoneSource: Int32,
                                    _ systemCallback: MrecAudioCallback?,
                                    _ systemUserData: UnsafeMutableRawPointer?,
                                    _ microphoneCallback: MrecAudioCallback?,
                                    _ microphoneUserData: UnsafeMutableRawPointer?) -> Int32 {
    guard #available(macOS 14.2, *) else {
        setLastError("CoreAudio process taps require macOS 14.2 or newer")
        return -1
    }

    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    if session.capture != nil || captureHealth() != 0 {
        setLastError("capture already running")
        return -3
    }

    guard microphoneSource == 0 || microphoneSource == 1 else {
        setLastError("unknown microphone source")
        return -9
    }
    if microphoneSource == 1, Permissions.microphoneStatus() != 1 {
        setLastError("microphone permission is not granted")
        return -2
    }
    let audioPermission = Permissions.audioCaptureStatus(timeout: 0.25)
    guard audioPermission == 1 else {
        setLastError(audioPermission == 2
            ? "system audio recording permission is denied"
            : "system audio recording permission is not granted yet; request it first")
        return -2
    }

    var pids: [pid_t] = []
    if let pidsPtr, pidCount > 0 {
        for index in 0..<pidCount { pids.append(pid_t(pidsPtr[index])) }
    }
    let global = globalMixdown != 0

    // With no explicit pids and no global request, capture every process currently
    // rendering audio, which keeps working while output is muted.
    if pids.isEmpty && !global {
        pids = AudioProcessRegistry.activeOutput().map(\.pid)
        if pids.isEmpty {
            setLastError("no process is currently playing audio; "
                + "pass explicit pids or set global_mixdown")
            return -5
        }
    }

    let configuration = SystemCaptureConfiguration(
        pids: pids,
        globalMixdown: global,
        mono: mono != 0,
        muteCapturedOutput: muteCapturedOutput != 0)
    let capture = TapCapture(stateBox: session.systemStateBox,
                             onConfigurationChange: scheduleCaptureRestart)

    // Start under a timeout rather than probing permission first: a probe builds
    // and tears down a second tap moments before this one, and that teardown races
    // the new tap's auto-start, producing a running but silent stream.
    //
    // A timeout here means the TCC grant is undetermined, which is when CoreAudio
    // blocks instead of failing.
    let outcome: Int32? = withTimeout(seconds: 6) {
        do {
            try capture.prepare(pids: configuration.pids,
                                globalMixdown: configuration.globalMixdown,
                                mono: configuration.mono,
                                muteCapturedOutput: configuration.muteCapturedOutput)
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
        session.systemStateBox.clear()
        session.microphoneStateBox.clear()
        setLastError("timed out starting capture; system audio recording "
            + "permission is most likely not granted yet — call "
            + "mrec_request_audio_permission() from a GUI process, or grant it "
            + "under System Settings > Privacy & Security > "
            + "Screen & System Audio Recording")
        return -2
    }
    guard outcome == 0 else {
        session.systemStateBox.clear()
        session.microphoneStateBox.clear()
        return outcome
    }

    session.systemStateBox.configure(callback: systemCallback, userData: systemUserData)
    session.microphoneStateBox.configure(callback: microphoneCallback,
                                         userData: microphoneUserData)
    do {
        try capture.startPrepared()
    } catch let error as CaptureError {
        session.systemStateBox.clear()
        session.microphoneStateBox.clear()
        setLastError(error.message)
        return error.status
    } catch {
        session.systemStateBox.clear()
        session.microphoneStateBox.clear()
        setLastError("capture start failed: \(error)")
        return -9
    }
    Permissions.noteAudioCaptureGranted()

    var microphone: MicrophoneCapture?
    if microphoneSource == 1 {
        do {
            microphone = try makeMicrophoneCapture()
        } catch let error as CaptureError {
            capture.stop()
            session.systemStateBox.clear()
            session.microphoneStateBox.clear()
            setLastError(error.message)
            return error.status
        } catch {
            capture.stop()
            session.systemStateBox.clear()
            session.microphoneStateBox.clear()
            setLastError("microphone setup failed: \(error)")
            return -9
        }
    }

    session.capture = capture
    session.microphone = microphone
    session.captureConfiguration = configuration
    setLastError("no error")
    setCaptureHealth(1) // MREC_CAPTURE_RUNNING
    return 0
}

@_cdecl("mrec_stop")
public func mrec_stop() -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard let capture = session.capture else {
        if captureHealth() == 3 {
            session.microphone?.stop()
            session.microphone = nil
            session.captureConfiguration = nil
            session.systemStateBox.clear()
            session.microphoneStateBox.clear()
            setCaptureHealth(0)
            return 0
        }
        return -4 /* NOT_RUNNING */
    }
    session.microphone?.stop()
    capture.stop()
    session.microphone = nil
    session.capture = nil
    session.captureConfiguration = nil
    session.systemStateBox.clear()
    session.microphoneStateBox.clear()
    setCaptureHealth(0) // MREC_CAPTURE_STOPPED
    return 0
}

@_cdecl("mrec_is_running")
public func mrec_is_running() -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }
    return session.capture != nil ? 1 : 0
}

@_cdecl("mrec_capture_health_status")
public func mrec_capture_health_status() -> Int32 {
    captureHealth()
}

@_cdecl("mrec_current_format")
public func mrec_current_format(_ sampleRate: UnsafeMutablePointer<Double>?,
                                  _ channels: UnsafeMutablePointer<UInt32>?) -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard session.capture != nil else { return -4 }
    let format = session.systemStateBox.currentFormat()
    sampleRate?.pointee = format.sampleRate
    channels?.pointee = format.channels
    return 0
}

@_cdecl("mrec_current_microphone_format")
public func mrec_current_microphone_format(_ sampleRate: UnsafeMutablePointer<Double>?,
                                             _ channels: UnsafeMutablePointer<UInt32>?) -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard session.microphone != nil else { return -4 }
    let format = session.microphoneStateBox.currentFormat()
    sampleRate?.pointee = format.sampleRate
    channels?.pointee = format.channels
    return 0
}
