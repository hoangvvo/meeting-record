import ApplicationServices
import Foundation

/// C ABI declared in native/include/meeting-record-detect.h.

public typealias MrecMeetingCallback = @convention(c) (
    UnsafeRawPointer?, Int32, UnsafeMutableRawPointer?
) -> Void

// MARK: - C struct layout
//
// Mirrors `mrec_meeting`. Swift cannot import the header, so these offsets are
// hand-written and must track meeting-record-detect.h. `layout_check.c` and
// `rust/tests/layout.rs` assert them.
//
//   platform          Int32     @ 0
//   pid               UInt32    @ 4
//   audio_pids[16]    UInt32    @ 8
//   audio_pid_count   size_t    @ 72   (8-aligned)
//   app_name[128]     char      @ 80
//   title[512]        char      @ 208
//   url[1024]         char      @ 720
//   is_using_mic      Int32     @ 1744
//   is_playing_audio  Int32     @ 1748
//   confidence        Int32     @ 1752
//   should_record     Int32     @ 1756
//   detected_at_ns    UInt64    @ 1760
//   total size                    1768

private enum Layout {
    static let platform = 0
    static let pid = 4
    static let audioPIDs = 8
    static let audioPIDCount = 72
    static let appName = 80
    static let title = 208
    static let url = 720
    static let isUsingMic = 1744
    static let isPlayingAudio = 1748
    static let confidence = 1752
    static let shouldRecord = 1756
    static let detectedAt = 1760
    static let stride = 1768

    static let appNameCapacity = 128
    static let titleCapacity = 512
    static let urlCapacity = 1024
    static let maxAudioPIDs = 16
}

private func writeString(_ value: String,
                         to base: UnsafeMutableRawPointer,
                         offset: Int,
                         capacity: Int) {
    let destination = base.advanced(by: offset).assumingMemoryBound(to: UInt8.self)
    destination.update(repeating: 0, count: capacity)
    let bytes = Array(value.utf8.prefix(capacity - 1))
    if !bytes.isEmpty { destination.update(from: bytes, count: bytes.count) }
}

private func hostTimeNanoseconds() -> UInt64 {
    var info = mach_timebase_info_data_t()
    guard mach_timebase_info(&info) == KERN_SUCCESS, info.denom != 0 else {
        return mach_absolute_time()
    }
    let ticks = mach_absolute_time()
    let numerator = UInt64(info.numer)
    let denominator = UInt64(info.denom)
    return (ticks / denominator) * numerator
        + (ticks % denominator) * numerator / denominator
}

/// Serialise one meeting into a zeroed C struct and pass it to `body`.
func withCMeeting<R>(_ meeting: DetectedMeeting,
                     _ body: (UnsafeRawPointer) -> R) -> R {
    let buffer = UnsafeMutableRawPointer.allocate(byteCount: Layout.stride,
                                                 alignment: 8)
    defer { buffer.deallocate() }
    buffer.initializeMemory(as: UInt8.self, repeating: 0, count: Layout.stride)
    fill(buffer, with: meeting)
    return body(UnsafeRawPointer(buffer))
}

private func fill(_ base: UnsafeMutableRawPointer, with meeting: DetectedMeeting) {
    base.advanced(by: Layout.platform)
        .assumingMemoryBound(to: Int32.self).pointee = meeting.platform.rawValue
    base.advanced(by: Layout.pid)
        .assumingMemoryBound(to: UInt32.self).pointee = UInt32(meeting.pid)

    let pids = meeting.audioPIDs.prefix(Layout.maxAudioPIDs)
    let pidBase = base.advanced(by: Layout.audioPIDs)
        .assumingMemoryBound(to: UInt32.self)
    for (index, pid) in pids.enumerated() { pidBase[index] = UInt32(pid) }
    base.advanced(by: Layout.audioPIDCount)
        .assumingMemoryBound(to: Int.self).pointee = pids.count

    writeString(meeting.appName, to: base, offset: Layout.appName,
                capacity: Layout.appNameCapacity)
    writeString(meeting.title, to: base, offset: Layout.title,
                capacity: Layout.titleCapacity)
    writeString(meeting.url, to: base, offset: Layout.url,
                capacity: Layout.urlCapacity)

    base.advanced(by: Layout.isUsingMic)
        .assumingMemoryBound(to: Int32.self).pointee = meeting.isUsingMic ? 1 : 0
    base.advanced(by: Layout.isPlayingAudio)
        .assumingMemoryBound(to: Int32.self).pointee = meeting.isPlayingAudio ? 1 : 0
    base.advanced(by: Layout.confidence)
        .assumingMemoryBound(to: Int32.self).pointee = meeting.confidence
    base.advanced(by: Layout.shouldRecord)
        .assumingMemoryBound(to: Int32.self).pointee =
        meeting.confidence >= MeetingDetector.recordThreshold ? 1 : 0
    base.advanced(by: Layout.detectedAt)
        .assumingMemoryBound(to: UInt64.self).pointee = hostTimeNanoseconds()
}

// MARK: - Scan

@_cdecl("mrec_scan")
public func mrec_scan(_ out: UnsafeMutableRawPointer?,
                                _ capacity: Int,
                                _ outCount: UnsafeMutablePointer<Int>?) -> Int32 {
    guard let out, let outCount else { return -9 /* INTERNAL */ }

    let meetings = MeetingDetector.scan()
    let count = min(meetings.count, capacity)

    for index in 0..<count {
        let slot = out.advanced(by: index * Layout.stride)
        slot.initializeMemory(as: UInt8.self, repeating: 0, count: Layout.stride)
        fill(slot, with: meetings[index])
    }

    outCount.pointee = count
    return meetings.count > capacity ? -10 /* BUFFER_TOO_SMALL */ : 0
}

// MARK: - Watching

private final class WatchSession {
    static let shared = WatchSession()
    let lock = NSLock()
    var watcher: MeetingWatcher?
}

@_cdecl("mrec_watch_start")
public func mrec_watch_start(_ callback: MrecMeetingCallback?,
                                       _ userData: UnsafeMutableRawPointer?) -> Int32 {
    guard let callback else { return -9 }
    let session = WatchSession.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    if session.watcher != nil { return -3 /* ALREADY_RUNNING */ }
    let watcher = MeetingWatcher(callback: callback, userData: userData)
    watcher.start()
    session.watcher = watcher
    return 0
}

@_cdecl("mrec_watch_stop")
public func mrec_watch_stop() -> Int32 {
    let session = WatchSession.shared
    session.lock.lock()
    defer { session.lock.unlock() }

    guard let watcher = session.watcher else { return -4 /* NOT_RUNNING */ }
    watcher.stop()
    session.watcher = nil
    return 0
}

@_cdecl("mrec_is_watching")
public func mrec_is_watching() -> Int32 {
    let session = WatchSession.shared
    session.lock.lock()
    defer { session.lock.unlock() }
    return session.watcher != nil ? 1 : 0
}

// MARK: - Accessibility permission

@_cdecl("mrec_accessibility_permission_status")
public func mrec_accessibility_permission_status() -> Int32 {
    Accessibility.isTrusted() ? 1 /* GRANTED */ : 2 /* DENIED */
}

@_cdecl("mrec_request_accessibility_permission")
public func mrec_request_accessibility_permission() -> Int32 {
    if Accessibility.isTrusted() { return 0 }
    Accessibility.openSettings()
    return 0
}

/// Allocated once and never freed: the C contract is a borrowed pointer with no
/// matching free, so per-call allocation would leak.
private let allPlatforms: [MeetingPlatform] = [
    .unknown, .zoom, .teams, .meet, .webex, .slack, .discord, .genericBrowser,
]

private func cachedStrings(_ value: @escaping (MeetingPlatform) -> String)
    -> [Int32: UnsafePointer<CChar>] {
    var table: [Int32: UnsafePointer<CChar>] = [:]
    for platform in allPlatforms {
        table[platform.rawValue] = UnsafePointer(strdup(value(platform)))
    }
    return table
}

private let platformIDs = cachedStrings { $0.id }

@_cdecl("mrec_platform_id")
public func mrec_platform_id(_ platform: Int32) -> UnsafePointer<CChar>? {
    platformIDs[platform] ?? platformIDs[MeetingPlatform.unknown.rawValue]
}


// MARK: - Recording a detected meeting

/// Start capture for a detected meeting.
///
/// Re-scans and prefers the current pids for the same platform, since the caller's
/// list may be stale. The passed-in list is the fallback.
@_cdecl("mrec_start_meeting")
public func mrec_start_meeting(_ meeting: UnsafeRawPointer?,
                               _ callback: MrecAudioCallback?,
                               _ userData: UnsafeMutableRawPointer?) -> Int32 {
    guard let meeting else { return -9 /* INTERNAL */ }

    let platform = meeting.advanced(by: Layout.platform)
        .assumingMemoryBound(to: Int32.self).pointee

    var pids: [UInt32] = []
    if let fresh = MeetingDetector.scan().first(where: { $0.platform.rawValue == platform }) {
        pids = fresh.audioPIDs.map(UInt32.init)
    } else {
        let count = min(meeting.advanced(by: Layout.audioPIDCount)
            .assumingMemoryBound(to: Int.self).pointee, Layout.maxAudioPIDs)
        let base = meeting.advanced(by: Layout.audioPIDs)
            .assumingMemoryBound(to: UInt32.self)
        pids = (0..<count).map { base[$0] }
    }
    guard !pids.isEmpty else { return -5 /* NO_PROCESSES */ }

    return pids.withUnsafeBufferPointer { buffer in
        mrec_start_raw(buffer.baseAddress, buffer.count,
                       0 /* not global */, 1 /* mono */, 0 /* do not mute */,
                       callback, userData)
    }
}
