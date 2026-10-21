import CoreAudio
import Foundation

/// C capture API. One process-wide session.

private final class Session {
    static let shared = Session()
    static let errorCapacity = 1024

    let lock = NSLock()
    let systemStateBox = CaptureStateBox()
    let microphoneStateBox = CaptureStateBox()
    var capture: TapCapture?
    var microphone: MicrophoneCapture?
    /// Set while a session is live, including while a source is being reopened.
    var captureConfiguration: SystemCaptureConfiguration?
    /// Bumped by every start and stop, so a retry never revives a later session.
    var generation = 0
    var trackSession: UInt64 = 0
    var capturePIDs: [pid_t] = []
    var microphoneEnabled = false
    var systemTargetExited = false
    var exitTimer: DispatchSourceTimer?
    let restartQueue = DispatchQueue(label: "meetingrecord.capture-restart")
    let microphoneRestartQueue = DispatchQueue(label: "meetingrecord.microphone-restart")
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

/// Ends the live session; the caller holds `Session.lock`.
private func releaseSession() {
    let session = Session.shared
    session.generation += 1
    session.exitTimer?.cancel()
    session.exitTimer = nil
    session.microphone?.stop()
    session.capture?.stop()
    session.microphone = nil
    session.capture = nil
    session.captureConfiguration = nil
    session.capturePIDs = []
    session.microphoneEnabled = false
    session.systemStateBox.clear()
    session.microphoneStateBox.clear()
    mrec_tracks_close()
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
    let pids = AudioAppRegistry.includingHelpers(configuration.pids).filter(AudioProcessRegistry.isAlive)
    try capture.start(pids: pids,
                      globalMixdown: configuration.globalMixdown,
                      mono: configuration.mono,
                      muteCapturedOutput: configuration.muteCapturedOutput)
    Session.shared.capturePIDs = pids
    return capture
}

private func makeMicrophoneCapture() throws -> MicrophoneCapture {
    let microphone = MicrophoneCapture(stateBox: Session.shared.microphoneStateBox,
                                       onConfigurationChange: scheduleMicrophoneRestart)
    try microphone.start()
    return microphone
}

/// A source for the shared recovery in tracks.c to reopen.
private final class Recovery {
    let generation: Int
    let track: mrec_track
    /// Reopens the source and returns its device name, if it has one.
    let reopen: () throws -> String?

    init(generation: Int, track: mrec_track, reopen: @escaping () throws -> String?) {
        self.generation = generation
        self.track = track
        self.reopen = reopen
    }
}

/// Reopens under the session lock, unless the session ended meanwhile.
private let reopenSource: mrec_track_attempt = { context, deviceName, capacity in
    guard let context else { return 0 }
    let recovery = Unmanaged<Recovery>.fromOpaque(context).takeUnretainedValue()
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }
    guard session.generation == recovery.generation,
          !(recovery.track == MREC_TRACK_SYSTEM_AUDIO && session.systemTargetExited) else { return 0 }
    do {
        if let name = try recovery.reopen(), let deviceName, capacity > 0 {
            let bytes = Array(name.utf8.prefix(capacity - 1))
            for (index, byte) in bytes.enumerated() {
                deviceName[index] = CChar(bitPattern: byte)
            }
            deviceName[bytes.count] = 0
        }
        return 1
    } catch let error as CaptureError {
        setLastError(error.message)
    } catch {
        setLastError(String(describing: error))
    }
    return 0
}

/// Sleeps without the lock, so stopping never waits for a retry.
private let waitForRetry: mrec_track_wait = { context, milliseconds in
    guard let context else { return 0 }
    let recovery = Unmanaged<Recovery>.fromOpaque(context).takeUnretainedValue()
    Thread.sleep(forTimeInterval: TimeInterval(milliseconds) / 1000)
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }
    return session.generation == recovery.generation
        && !(recovery.track == MREC_TRACK_SYSTEM_AUDIO && session.systemTargetExited) ? 1 : 0
}

/// Never stops the recording: retries until the source returns or the session ends.
private func recover(_ track: mrec_track, reason: mrec_track_reason, message: String,
                     generation: Int, trackSession: UInt64, reopen: @escaping () throws -> String?) {
    let context = Unmanaged.passRetained(Recovery(generation: generation, track: track, reopen: reopen))
    defer { context.release() }
    _ = mrec_track_recover(trackSession, track, 0, reason, message, reopenSource, waitForRetry,
                           context.toOpaque())
}

private func scheduleCaptureRestart(_ identifier: UUID, _ reason: mrec_track_reason,
                                    _ detail: String) {
    let session = Session.shared
    session.restartQueue.async {
        session.lock.lock()
        guard let current = session.capture,
              current.identifier == identifier,
              let configuration = session.captureConfiguration else {
            session.lock.unlock()
            return
        }
        let generation = session.generation
        let trackSession = session.trackSession
        setLastError(detail)
        current.stop()
        session.capture = nil
        session.lock.unlock()

        recover(MREC_TRACK_SYSTEM_AUDIO, reason: reason,
                message: "system audio interrupted: \(detail)", generation: generation, trackSession: trackSession) {
            session.capture = try makeTapCapture(configuration)
            return nil
        }
    }
}

private func scheduleMicrophoneRestart(_ identifier: UUID, _ reason: mrec_track_reason,
                                       _ detail: String) {
    let session = Session.shared
    session.microphoneRestartQueue.async {
        session.lock.lock()
        guard let current = session.microphone,
              current.identifier == identifier,
              session.captureConfiguration != nil else {
            session.lock.unlock()
            return
        }
        let generation = session.generation
        let trackSession = session.trackSession
        setLastError(detail)
        current.stop()
        session.microphone = nil
        session.lock.unlock()

        recover(MREC_TRACK_MICROPHONE, reason: reason,
                message: "microphone interrupted: \(detail)", generation: generation, trackSession: trackSession) {
            session.microphone = try makeMicrophoneCapture()
            return MicrophoneCapture.defaultInputName()
        }
    }
}

/// Follow helper replacement within the selected processes' lifetime. A relaunched
/// app requires a new capture session after the selected processes exit.
private func watchSources(generation: Int) -> DispatchSourceTimer {
    let session = Session.shared
    let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
    timer.schedule(deadline: .now() + 1, repeating: 1)
    timer.setEventHandler {
        session.lock.lock()
        defer { session.lock.unlock() }
        guard session.generation == generation,
              let configuration = session.captureConfiguration else { return }
        let targets = configuration.pids.filter(AudioProcessRegistry.isAlive)
        if targets.isEmpty {
            session.exitTimer?.cancel()
            session.exitTimer = nil
            session.systemTargetExited = true
            session.capture?.stop()
            session.capture = nil
            mrec_track_interrupted(session.trackSession, MREC_TRACK_SYSTEM_AUDIO,
                                   MREC_TRACK_WHOLE, MREC_TRACK_REASON_APP_EXITED,
                                   "the captured apps exited; start a new capture for a relaunched app")
            return
        }
        let pids = AudioAppRegistry.includingHelpers(targets).filter(AudioProcessRegistry.isAlive)
        if pids != session.capturePIDs, let capture = session.capture {
            scheduleCaptureRestart(capture.identifier, MREC_TRACK_REASON_UNKNOWN,
                                   "the app's audio processes changed")
        }
    }
    timer.resume()
    return timer
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
    writeProcesses(AudioProcessRegistry.all(), to: out, capacity: capacity, outCount: outCount)
}

@_cdecl("mrec_list_audio_apps")
public func mrec_list_audio_apps(_ out: UnsafeMutableRawPointer?,
                                   _ capacity: Int,
                                   _ outCount: UnsafeMutablePointer<Int>?) -> Int32 {
    writeProcesses(AudioAppRegistry.all(), to: out, capacity: capacity, outCount: outCount)
}

private func writeProcesses(_ processes: [AudioProcess],
                            to out: UnsafeMutableRawPointer?,
                            capacity: Int,
                            outCount: UnsafeMutablePointer<Int>?) -> Int32 {
    guard let out, let outCount else { return -9 /* INTERNAL */ }

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
                          muteCapturedOutput, 0, callback, userData, nil, nil, nil, nil)
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
                                    _ microphoneUserData: UnsafeMutableRawPointer?,
                                    _ eventCallback: mrec_track_event_callback?,
                                    _ eventUserData: UnsafeMutableRawPointer?) -> Int32 {
    guard #available(macOS 14.2, *) else {
        setLastError("CoreAudio process taps require macOS 14.2 or newer")
        return -1
    }

    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    if session.captureConfiguration != nil {
        setLastError("capture already running")
        return -3
    }

    guard (0...2).contains(microphoneSource) else {
        setLastError("unknown microphone source")
        return -9
    }
    if microphoneSource != 0, Permissions.microphoneStatus() != 1 {
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
        for index in 0..<pidCount {
            guard let pid = pid_t(exactly: pidsPtr[index]), pid > 0 else {
                setLastError("process pid is out of range")
                return -9
            }
            pids.append(pid)
        }
    }
    let global = globalMixdown != 0 && pids.isEmpty

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
        pids: Array(Set(pids)).sorted(),
        globalMixdown: global,
        mono: mono != 0,
        muteCapturedOutput: muteCapturedOutput != 0)
    let capture = TapCapture(stateBox: session.systemStateBox,
                             onConfigurationChange: scheduleCaptureRestart)

    // Avoid a separate permission probe: its teardown can race this tap.
    // Time out if CoreAudio blocks on an unanswered permission request.
    let capturePIDs = AudioAppRegistry.includingHelpers(configuration.pids)
    let outcome: Int32? = withTimeout(seconds: 6) {
        do {
            try capture.prepare(pids: capturePIDs,
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
    if microphoneSource != 0 {
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
    session.capturePIDs = capturePIDs
    session.systemTargetExited = false
    session.microphoneEnabled = microphoneSource != 0
    session.generation += 1
    session.trackSession = mrec_tracks_open(eventCallback, eventUserData)
    guard mrec_track_started(session.trackSession, MREC_TRACK_SYSTEM_AUDIO, 1, nil) != 0 else {
        releaseSession()
        setLastError("could not allocate track state")
        return -9
    }
    if microphone != nil {
        guard mrec_track_started(session.trackSession, MREC_TRACK_MICROPHONE, 1,
                                 MicrophoneCapture.defaultInputName() ?? "") != 0 else {
            releaseSession()
            setLastError("could not allocate track state")
            return -9
        }
    }
    if !configuration.globalMixdown {
        session.exitTimer = watchSources(generation: session.generation)
    }
    setLastError("no error")
    return 0
}

@_cdecl("mrec_stop")
public func mrec_stop() -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard session.captureConfiguration != nil else { return -4 /* NOT_RUNNING */ }
    releaseSession()
    return 0
}

@_cdecl("mrec_is_running")
public func mrec_is_running() -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }
    return session.captureConfiguration != nil ? 1 : 0
}

@_cdecl("mrec_current_format")
public func mrec_current_format(_ sampleRate: UnsafeMutablePointer<Double>?,
                                  _ channels: UnsafeMutablePointer<UInt32>?) -> Int32 {
    let session = Session.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard session.captureConfiguration != nil else { return -4 }
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

    guard session.captureConfiguration != nil, session.microphoneEnabled else { return -4 }
    let format = session.microphoneStateBox.currentFormat()
    sampleRate?.pointee = format.sampleRate
    channels?.pointee = format.channels
    return 0
}
