import Foundation

/// Watcher-only snapshots; foreground, audio-active and call UIs stay fresh.
final class MeetingWindowCache {
    private struct Entry {
        var identity: UInt64
        var platform: MeetingPlatform?
        var readAt: TimeInterval
        var contents: [BrowserWindowContent]
        var hasCallEvidence: Bool
    }

    private let idleInterval: TimeInterval
    private let identity: (pid_t) -> UInt64?
    private let now: () -> TimeInterval
    private var entries: [pid_t: Entry] = [:]

    init(idleInterval: TimeInterval = 30,
         identity: @escaping (pid_t) -> UInt64? = ProcessTree.identity,
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.idleInterval = idleInterval
        self.identity = identity
        self.now = now
    }

    func read(pid: pid_t, platform: MeetingPlatform?, forceFresh: Bool,
              _ reader: () -> [BrowserWindowContent]) -> [BrowserWindowContent] {
        guard let lifetime = identity(pid) else {
            entries[pid] = nil
            return reader()
        }
        let time = now()
        if let entry = entries[pid], entry.identity == lifetime, entry.platform == platform,
           !forceFresh, !entry.hasCallEvidence, time - entry.readAt < idleInterval {
            return entry.contents
        }
        let contents = reader()
        let hasCallEvidence = contents.contains {
            $0.callSignals != 0 || (platform == nil && MeetingCatalog.browserWindow($0).platform != nil)
        }
        entries[pid] = Entry(identity: lifetime, platform: platform, readAt: time,
                             contents: contents, hasCallEvidence: hasCallEvidence)
        return contents
    }

    func retain(pids: Set<pid_t>) { entries = entries.filter { pids.contains($0.key) } }
    func removeAll() { entries.removeAll() }
}
