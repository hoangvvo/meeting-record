// Fake audio sources for session tests without permissions or device changes.
import CoreAudio
import Foundation

public typealias MrecAudioCallback = @convention(c) (
    UnsafePointer<Float>?, UInt32, UInt32, Double, UInt64, UnsafeMutableRawPointer?
) -> Void

final class CaptureStateBox {
    private let lock = NSLock()
    private var callback: MrecAudioCallback?
    private var userData: UnsafeMutableRawPointer?
    private var sampleRate = 0.0
    private var channels: UInt32 = 0

    func configure(callback: MrecAudioCallback?, userData: UnsafeMutableRawPointer?) {
        lock.lock()
        defer { lock.unlock() }
        self.callback = callback
        self.userData = userData
    }

    func updateFormat(sampleRate: Double, channels: UInt32) {
        lock.lock()
        defer { lock.unlock() }
        self.sampleRate = sampleRate
        self.channels = channels
    }

    func clear() {
        lock.lock()
        defer { lock.unlock() }
        callback = nil
        userData = nil
        sampleRate = 0
        channels = 0
    }

    func currentFormat() -> (sampleRate: Double, channels: UInt32) {
        lock.lock()
        defer { lock.unlock() }
        return (sampleRate, channels)
    }

    func emit() {
        lock.lock()
        let callback = callback
        let data = userData
        let rate = sampleRate
        let count = channels
        lock.unlock()
        let samples = Array(repeating: Float(0.25), count: Int(count) * 2)
        samples.withUnsafeBufferPointer { callback?($0.baseAddress, 2, count, rate, 42, data) }
    }
}

struct TestSourceRecord {
    var pids: [pid_t] = []
    var global = false
    var prepared = false
    var started = false
    var stops = 0
}

struct TestAudioPacket {
    var frames: UInt32
    var channels: UInt32
    var sampleRate: Double
    var time: UInt64
}

enum TestSources {
    static let lock = NSLock()
    static var alive: Set<pid_t> = [123, 456]
    static var helpers: [pid_t] = [456]
    static var exitDuringHelperLookup = false
    static var prepared: [[pid_t]] = []
    static weak var tap: TapCapture?
    static weak var microphone: MicrophoneCapture?
    static var taps: [UUID: TestSourceRecord] = [:]
    static var microphones: [UUID: TestSourceRecord] = [:]
    static var tapAvailable = true
    static var microphoneAvailable = true
    static var tapPrepareFailures = 0
    static var tapStartFailures = 0
    static var microphoneStartFailures = 0
    static var tapAttempts = 0
    static var microphoneAttempts = 0
    static var preparationTimesOut = false
    static var audioPermission: Int32 = 1
    static var microphonePermission: Int32 = 1
    static var tapRate = 48000.0
    static var microphoneRate = 44100.0
    static var microphoneName = "Test microphone"
    static var events: [mrec_track_event] = []
    static var systemPackets: [TestAudioPacket] = []
    static var microphonePackets: [TestAudioPacket] = []
    static var deviceListeners: [AudioObjectPropertySelector: AudioObjectPropertyListenerBlock] = [:]

    static func read<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    static func reset() {
        read {
            precondition(deviceListeners.isEmpty, "a device listener outlived its session")
            alive = [123, 456]
            helpers = [456]
            exitDuringHelperLookup = false
            prepared = []
            tap = nil
            microphone = nil
            taps = [:]
            microphones = [:]
            tapAvailable = true
            microphoneAvailable = true
            tapPrepareFailures = 0
            tapStartFailures = 0
            microphoneStartFailures = 0
            tapAttempts = 0
            microphoneAttempts = 0
            preparationTimesOut = false
            audioPermission = 1
            microphonePermission = 1
            tapRate = 48000
            microphoneRate = 44100
            microphoneName = "Test microphone"
            events = []
            systemPackets = []
            microphonePackets = []
        }
    }

    static func deviceChanged(_ selector: AudioObjectPropertySelector) {
        let listener = read { deviceListeners[selector] }
        var address = AudioObjectPropertyAddress(mSelector: selector,
                                                mScope: kAudioObjectPropertyScopeGlobal,
                                                mElement: kAudioObjectPropertyElementMain)
        withUnsafePointer(to: &address) { listener?(1, $0) }
    }
}

@discardableResult
func AudioObjectAddPropertyListenerBlock(_ object: AudioObjectID,
                                        _ address: UnsafePointer<AudioObjectPropertyAddress>,
                                        _ queue: DispatchQueue,
                                        _ block: @escaping AudioObjectPropertyListenerBlock) -> OSStatus {
    precondition(object == kAudioObjectSystemObject)
    TestSources.read {
        precondition(TestSources.deviceListeners[address.pointee.mSelector] == nil)
        TestSources.deviceListeners[address.pointee.mSelector] = block
    }
    return noErr
}

@discardableResult
func AudioObjectRemovePropertyListenerBlock(_ object: AudioObjectID,
                                           _ address: UnsafePointer<AudioObjectPropertyAddress>,
                                           _ queue: DispatchQueue,
                                           _ block: AudioObjectPropertyListenerBlock) -> OSStatus {
    precondition(object == kAudioObjectSystemObject)
    TestSources.read {
        precondition(TestSources.deviceListeners.removeValue(forKey: address.pointee.mSelector) != nil)
    }
    return noErr
}

final class TapCapture {
    let identifier = UUID()
    let change: (UUID, mrec_track_reason, String) -> Void
    private let stateBox: CaptureStateBox

    init(
        stateBox: CaptureStateBox,
        onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void
    ) {
        self.stateBox = stateBox
        change = onConfigurationChange
        TestSources.read { TestSources.taps[identifier] = TestSourceRecord() }
    }

    deinit { stop() }

    func start(pids: [pid_t], globalMixdown: Bool, mono: Bool, muteCapturedOutput: Bool) throws {
        try prepare(
            pids: pids, globalMixdown: globalMixdown, mono: mono,
            muteCapturedOutput: muteCapturedOutput)
        try startPrepared()
    }

    func prepare(pids: [pid_t], globalMixdown: Bool, mono: Bool, muteCapturedOutput: Bool) throws {
        let rate = try TestSources.read {
            TestSources.tapAttempts += 1
            TestSources.prepared.append(pids)
            TestSources.taps[identifier]?.pids = pids
            TestSources.taps[identifier]?.global = globalMixdown
            guard TestSources.tapAvailable else { throw CaptureError.tapUnavailable }
            if TestSources.tapPrepareFailures > 0 {
                TestSources.tapPrepareFailures -= 1
                throw CaptureError.tapUnavailable
            }
            TestSources.taps[identifier]?.prepared = true
            return TestSources.tapRate
        }
        stateBox.updateFormat(sampleRate: rate, channels: mono ? 1 : 2)
    }

    func startPrepared() throws {
        try TestSources.read {
            if TestSources.tapStartFailures > 0 {
                TestSources.tapStartFailures -= 1
                throw CaptureError.startFailed
            }
            TestSources.taps[identifier]?.started = true
            TestSources.tap = self
        }
    }

    func stop() {
        TestSources.read {
            guard let record = TestSources.taps[identifier], record.prepared || record.started
            else { return }
            TestSources.taps[identifier]?.prepared = false
            TestSources.taps[identifier]?.started = false
            TestSources.taps[identifier]?.stops += 1
        }
    }

    func emit() {
        if TestSources.read({ TestSources.taps[identifier]?.started == true }) { stateBox.emit() }
    }
}

final class MicrophoneCapture {
    let identifier = UUID()
    let change: (UUID, mrec_track_reason, String) -> Void
    private let stateBox: CaptureStateBox

    init(
        stateBox: CaptureStateBox,
        onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void
    ) {
        self.stateBox = stateBox
        change = onConfigurationChange
        TestSources.read { TestSources.microphones[identifier] = TestSourceRecord() }
    }

    deinit { stop() }

    func start() throws {
        let rate = try TestSources.read {
            TestSources.microphoneAttempts += 1
            guard TestSources.microphoneAvailable else { throw CaptureError.microphoneUnavailable }
            if TestSources.microphoneStartFailures > 0 {
                TestSources.microphoneStartFailures -= 1
                throw CaptureError.microphoneUnavailable
            }
            TestSources.microphones[identifier]?.started = true
            TestSources.microphone = self
            return TestSources.microphoneRate
        }
        stateBox.updateFormat(sampleRate: rate, channels: 1)
    }

    func stop() {
        TestSources.read {
            guard TestSources.microphones[identifier]?.started == true else { return }
            TestSources.microphones[identifier]?.started = false
            TestSources.microphones[identifier]?.stops += 1
        }
    }

    func emit() {
        if TestSources.read({ TestSources.microphones[identifier]?.started == true }) {
            stateBox.emit()
        }
    }

    static func defaultInputName() -> String? { TestSources.read { TestSources.microphoneName } }
}

struct AudioProcess {
    var pid: pid_t = 123
    var isRunningOutput = true
    var isRunningInput = false
    var bundleID = "test"
    var name = "test"
}

enum AudioProcessRegistry {
    static func all() -> [AudioProcess] { [AudioProcess()] }
    static func activeOutput() -> [AudioProcess] { all() }
    static func isAlive(_ pid: pid_t) -> Bool {
        TestSources.read { TestSources.alive.contains(pid) }
    }
}

enum AudioAppRegistry {
    static func all() -> [AudioProcess] { AudioProcessRegistry.all() }
    static func includingHelpers(_ pids: [pid_t]) -> [pid_t] {
        TestSources.read {
            let expanded = pids.isEmpty ? [] : Array(Set(pids + TestSources.helpers)).sorted()
            if TestSources.exitDuringHelperLookup {
                TestSources.exitDuringHelperLookup = false
                TestSources.alive = []
                TestSources.helpers = []
            }
            return expanded
        }
    }
}

enum Permissions {
    static func microphoneStatus() -> Int32 {
        TestSources.read { TestSources.microphonePermission }
    }
    static func audioCaptureStatus(timeout: TimeInterval = 0.05) -> Int32 {
        TestSources.read { TestSources.audioPermission }
    }
    static func requestAudioCapture() -> Int32 { 0 }
    static func requestMicrophone() -> Int32 { 0 }
    static func noteAudioCaptureGranted() {}
}

enum CaptureError: Error {
    case noProcesses
    case tapUnavailable
    case startFailed
    case microphoneUnavailable
    var message: String {
        switch self {
        case .noProcesses: return "no selected processes remain"
        case .tapUnavailable: return "system audio unavailable"
        case .startFailed: return "system audio start failed"
        case .microphoneUnavailable: return "microphone unavailable"
        }
    }
    var status: Int32 {
        switch self {
        case .noProcesses: return -5
        case .tapUnavailable: return -6
        case .startFailed: return -8
        case .microphoneUnavailable: return -7
        }
    }
}

func withTimeout<T>(seconds: TimeInterval, _ work: @escaping () -> T) -> T? {
    let timeout = TestSources.read {
        let timeout = TestSources.preparationTimesOut
        TestSources.preparationTimesOut = false
        return timeout
    }
    return timeout ? nil : work()
}
