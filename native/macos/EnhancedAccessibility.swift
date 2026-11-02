import Foundation

/// Own one activation per process lifetime, shared by watchers and direct scans.
final class EnhancedAccessibility {
    private struct Entry {
        let identity: UInt64
        var readers: Int
        var lastRead: TimeInterval
        var releasing = false
    }

    private let lock = NSLock()
    private let read: (pid_t) -> Bool?
    private let write: (pid_t, Bool) -> Bool
    private let identity: (pid_t) -> UInt64?
    private let now: () -> TimeInterval
    private let idleTimeout: TimeInterval
    private let cleanupInterval: TimeInterval?
    private var entries: [pid_t: Entry] = [:]
    private var timer: DispatchSourceTimer?

    init(idleTimeout: TimeInterval = 60, cleanupInterval: TimeInterval? = nil,
         read: @escaping (pid_t) -> Bool?, write: @escaping (pid_t, Bool) -> Bool,
         identity: @escaping (pid_t) -> UInt64?,
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.idleTimeout = idleTimeout
        self.read = read
        self.write = write
        self.identity = identity
        self.now = now
        self.cleanupInterval = cleanupInterval
    }

    deinit { timer?.cancel() }

    func withRead<T>(pid: pid_t, _ body: () throws -> T) rethrows -> T {
        let token = beginRead(pid: pid)
        defer { if let token { endRead(pid: pid, identity: token) } }
        return try body()
    }

    private func beginRead(pid: pid_t) -> UInt64? {
        lock.lock()
        defer { lock.unlock() }
        guard let current = identity(pid) else { return nil }
        if let entry = entries[pid], entry.identity != current { remove(pid: pid) }
        if var entry = entries[pid] {
            entry.readers += 1
            entry.lastRead = now()
            entries[pid] = entry
            return current
        }
        // Unknown or pre-enabled state belongs to someone else. Repeated true
        // writes are not idempotent on every accessibility implementation.
        guard read(pid) == false, identity(pid) == current, write(pid, true) else { return nil }
        entries[pid] = Entry(identity: current, readers: 1, lastRead: now())
        if timer == nil, let interval = cleanupInterval {
            let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
            timer.schedule(deadline: .now() + interval, repeating: interval)
            timer.setEventHandler { [weak self] in self?.cleanupExpired() }
            timer.resume()
            self.timer = timer
        }
        return current
    }

    private func endRead(pid: pid_t, identity: UInt64) {
        lock.lock()
        defer { lock.unlock() }
        guard var entry = entries[pid], entry.identity == identity else { return }
        entry.readers -= 1
        entry.lastRead = now()
        entries[pid] = entry
        if entry.releasing && entry.readers == 0 { release(pid: pid, entry: entry) }
    }

    func cleanupExpired() {
        lock.lock()
        defer { lock.unlock() }
        let time = now()
        for (pid, entry) in entries where entry.readers == 0 &&
            (entry.releasing || time - entry.lastRead >= idleTimeout) {
            release(pid: pid, entry: entry)
        }
    }

    func releaseAll() {
        lock.lock()
        defer { lock.unlock() }
        for (pid, var entry) in entries {
            entry.releasing = true
            entries[pid] = entry
            if entry.readers == 0 { release(pid: pid, entry: entry) }
        }
    }

    /// Called under the lock. Failed resets remain eligible for a later retry.
    private func release(pid: pid_t, entry: Entry) {
        guard identity(pid) == entry.identity else { remove(pid: pid); return }
        // Balance our successful enable even if activation is still pending.
        if write(pid, false) {
            remove(pid: pid)
        } else {
            var pending = entry
            pending.releasing = true
            entries[pid] = pending
        }
    }

    private func remove(pid: pid_t) {
        entries[pid] = nil
        if entries.isEmpty {
            timer?.cancel()
            timer = nil
        }
    }
}
