// Fake OS sources for testing the actual macOS session coordinator without TCC
// prompts or changing the user's audio devices.
import CoreAudio
import Foundation

public typealias MrecAudioCallback = @convention(c) (
    UnsafePointer<Float>?, UInt32, UInt32, Double, UInt64, UnsafeMutableRawPointer?
) -> Void
final class CaptureStateBox {
    func configure(callback: MrecAudioCallback?, userData: UnsafeMutableRawPointer?) {}
    func clear() {}
    func currentFormat() -> (sampleRate: Double, channels: UInt32) { (48000, 1) }
}
enum TestSources {
    static let lock = NSLock()
    static var alive: Set<pid_t> = [123, 456]
    static var helpers: [pid_t] = [456]
    static var prepared: [[pid_t]] = []
    static var microphone: MicrophoneCapture?
    static var microphoneAvailable = true
    static var events: [mrec_track_event] = []
    static func read<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}
final class TapCapture {
    let identifier = UUID()
    init(stateBox: CaptureStateBox, onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void) {}
    func start(pids: [pid_t], globalMixdown: Bool, mono: Bool, muteCapturedOutput: Bool) throws {
        try prepare(pids: pids, globalMixdown: globalMixdown, mono: mono, muteCapturedOutput: muteCapturedOutput)
    }
    func prepare(pids: [pid_t], globalMixdown: Bool, mono: Bool, muteCapturedOutput: Bool) throws {
        TestSources.read { TestSources.prepared.append(pids) }
    }
    func startPrepared() throws {}
    func stop() {}
}
final class MicrophoneCapture {
    let identifier = UUID()
    let change: (UUID, mrec_track_reason, String) -> Void
    init(stateBox: CaptureStateBox, onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void) {
        change = onConfigurationChange
    }
    func start() throws {
        try TestSources.read {
            guard TestSources.microphoneAvailable else { throw CaptureError.unavailable }
            TestSources.microphone = self
        }
    }
    func stop() {}
    static func defaultInputName() -> String? { "Test microphone" }
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
    static func isAlive(_ pid: pid_t) -> Bool { TestSources.read { TestSources.alive.contains(pid) } }
}
enum AudioAppRegistry {
    static func all() -> [AudioProcess] { AudioProcessRegistry.all() }
    static func includingHelpers(_ pids: [pid_t]) -> [pid_t] {
        TestSources.read { Array(Set(pids + TestSources.helpers)).sorted() }
    }
}
enum Permissions {
    static func microphoneStatus() -> Int32 { 1 }
    static func audioCaptureStatus(timeout: TimeInterval = 0.05) -> Int32 { 1 }
    static func requestAudioCapture() -> Int32 { 0 }
    static func requestMicrophone() -> Int32 { 0 }
    static func noteAudioCaptureGranted() {}
}
enum CaptureError: Error {
    case unavailable
    var message: String { "no microphone" }
    var status: Int32 { -7 }
}
func withTimeout<T>(seconds: TimeInterval, _ work: @escaping () -> T) -> T? { work() }
