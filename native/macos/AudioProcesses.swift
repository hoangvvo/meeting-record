import CoreAudio
import Foundation

/// Thin wrappers over the CoreAudio HAL property API, plus enumeration of the
/// system's audio process objects.
///
/// `kAudioHardwarePropertyProcessObjectList` is the only dependable way to learn
/// which applications are actually producing sound — window titles and
/// `NSRunningApplication` tell you nothing about audio IO.
enum HAL {
    static let system = AudioObjectID(kAudioObjectSystemObject)

    static func address(_ selector: AudioObjectPropertySelector,
                        scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal)
        -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector,
                                   mScope: scope,
                                   mElement: kAudioObjectPropertyElementMain)
    }

    static func dataSize(_ object: AudioObjectID,
                         _ selector: AudioObjectPropertySelector,
                         scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal)
        -> UInt32? {
        var addr = address(selector, scope: scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(object, &addr, 0, nil, &size) == noErr else {
            return nil
        }
        return size
    }

    static func value<T>(_ object: AudioObjectID,
                         _ selector: AudioObjectPropertySelector,
                         scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                         default def: T) -> T {
        var addr = address(selector, scope: scope)
        var size = UInt32(MemoryLayout<T>.size)
        var out = def
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &out) == noErr else {
            return def
        }
        return out
    }

    static func string(_ object: AudioObjectID,
                       _ selector: AudioObjectPropertySelector) -> String? {
        var addr = address(selector)
        var size = UInt32(MemoryLayout<CFString?>.size)
        var raw: Unmanaged<CFString>?
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &raw) == noErr,
              let cf = raw?.takeRetainedValue() else { return nil }
        return cf as String
    }

    /// Read a variable-length property into an array of trivial values.
    ///
    /// Goes through raw memory rather than pre-filling a Swift array: there is no
    /// generic way to synthesise a zero `T`, and bit-casting a fixed-width zero
    /// into `T` traps whenever the widths differ.
    static func array<T>(_ object: AudioObjectID,
                         _ selector: AudioObjectPropertySelector,
                         scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                         of _: T.Type) -> [T] {
        var addr = address(selector, scope: scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(object, &addr, 0, nil, &size) == noErr,
              size >= UInt32(MemoryLayout<T>.size) else { return [] }

        let count = Int(size) / MemoryLayout<T>.stride
        let buffer = UnsafeMutableBufferPointer<T>.allocate(capacity: count)
        defer { buffer.deallocate() }

        var readSize = UInt32(count * MemoryLayout<T>.stride)
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &readSize,
                                         buffer.baseAddress!) == noErr else { return [] }

        let received = Int(readSize) / MemoryLayout<T>.stride
        return Array(UnsafeBufferPointer(start: buffer.baseAddress!,
                                         count: min(received, count)))
    }

    /// Default output device, and its UID (needed as the aggregate's clock anchor).
    static func defaultOutputDevice() -> (id: AudioObjectID, uid: String)? {
        let id = value(system, kAudioHardwarePropertyDefaultOutputDevice,
                       default: AudioObjectID(kAudioObjectUnknown))
        guard id != kAudioObjectUnknown, let uid = string(id, kAudioDevicePropertyDeviceUID) else {
            return nil
        }
        return (id, uid)
    }

    /// Translate a Unix pid into the HAL's process *object* id.
    ///
    /// Passing a raw pid where an AudioObjectID is expected fails with
    /// `kAudioHardwareBadObjectError` ('!obj') — the two namespaces are unrelated.
    static func processObject(forPID pid: pid_t) -> AudioObjectID? {
        var addr = address(kAudioHardwarePropertyTranslatePIDToProcessObject)
        var inPID = pid
        var out = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let err = AudioObjectGetPropertyData(system, &addr,
                                             UInt32(MemoryLayout<pid_t>.size), &inPID,
                                             &size, &out)
        guard err == noErr, out != kAudioObjectUnknown else { return nil }
        return out
    }
}

/// A process the HAL knows is doing audio IO.
struct AudioProcess {
    var objectID: AudioObjectID
    var pid: pid_t
    var bundleID: String
    var name: String
    var isRunningOutput: Bool
    var isRunningInput: Bool
}

enum AudioProcessRegistry {
    /// Every live process the HAL knows is doing audio IO.
    ///
    /// Dead processes are excluded here rather than left to the caller. The HAL
    /// keeps process objects after the process exits and still reports
    /// `isRunningOutput == true` for them, so a caller that trusts this list would
    /// build a tap over corpses and capture silence — with no error anywhere.
    static func all() -> [AudioProcess] {
        HAL.array(HAL.system, kAudioHardwarePropertyProcessObjectList, of: AudioObjectID.self)
            .compactMap { obj in
                let pid = pid_t(HAL.value(obj, kAudioProcessPropertyPID, default: Int32(-1)))
                guard pid > 0, isAlive(pid) else { return nil }
                let bundleID = HAL.string(obj, kAudioProcessPropertyBundleID) ?? ""
                return AudioProcess(
                    objectID: obj,
                    pid: pid,
                    bundleID: bundleID,
                    name: executableName(pid: pid, bundleID: bundleID),
                    isRunningOutput: HAL.value(obj, kAudioProcessPropertyIsRunningOutput,
                                               default: UInt32(0)) != 0,
                    isRunningInput: HAL.value(obj, kAudioProcessPropertyIsRunningInput,
                                              default: UInt32(0)) != 0
                )
            }
    }

    /// Processes currently rendering audio — the natural capture target.
    /// `all()` has already excluded dead processes.
    static func activeOutput() -> [AudioProcess] {
        all().filter { $0.isRunningOutput && $0.pid != getpid() }
    }

    /// Signal 0 performs permission and existence checks without delivering.
    /// EPERM means the process exists but belongs to someone else — still alive.
    ///
    /// Load-bearing: see the note on `all()`.
    static func isAlive(_ pid: pid_t) -> Bool {
        kill(pid, 0) == 0 || errno == EPERM
    }

    /// The HAL exposes no process name and helper processes have no bundle id, so
    /// read the executable name from the kernel.
    private static func executableName(pid: pid_t, bundleID: String) -> String {
        var buf = [CChar](repeating: 0, count: 4096)
        if proc_pidpath(pid, &buf, UInt32(buf.count)) > 0 {
            let path = String(cString: buf)
            if !path.isEmpty {
                return (path as NSString).lastPathComponent
            }
        }
        return bundleID
    }
}
