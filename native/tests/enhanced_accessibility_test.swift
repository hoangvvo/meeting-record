import Foundation

@main struct EnhancedAccessibilityTests {
    static func main() {
        var time: TimeInterval = 0
        var flags: [pid_t: Bool] = [1: false, 2: true]
        var identities: [pid_t: UInt64] = [1: 10, 2: 20, 3: 30]
        var writes: [(pid_t, Bool)] = []
        var failEnable = false
        var failReset = false
        let tracker = EnhancedAccessibility(
            read: { flags[$0] },
            write: { pid, enabled in
                writes.append((pid, enabled))
                if enabled ? failEnable : failReset { return false }
                flags[pid] = enabled
                return true
            }, identity: { identities[$0] }, now: { time })

        // Repeated and nested reads hold one activation, including while stopping.
        tracker.withRead(pid: 1) {
            tracker.withRead(pid: 1) { precondition(flags[1] == true) }
            precondition(writes.count == 1)
            tracker.releaseAll()
            precondition(flags[1] == true)
        }
        precondition(flags[1] == false && writes.count == 2)

        // A pre-enabled or unreadable attribute is never ours to change.
        tracker.withRead(pid: 2) {}
        tracker.withRead(pid: 3) {}
        tracker.releaseAll()
        precondition(flags[2] == true && writes.count == 2)

        tracker.withRead(pid: 1) {}
        time = 59
        tracker.cleanupExpired()
        precondition(flags[1] == true)
        tracker.withRead(pid: 1) {}
        time = 118
        tracker.cleanupExpired()
        precondition(flags[1] == true)
        time = 119
        tracker.cleanupExpired()
        precondition(flags[1] == false)

        tracker.withRead(pid: 1) {
            time = 200
            tracker.cleanupExpired()
            precondition(flags[1] == true, "expiry must not interrupt a reader")
        }
        time = 260
        tracker.cleanupExpired()
        precondition(flags[1] == false)

        failEnable = true
        let beforeFailure = writes.count
        tracker.withRead(pid: 1) {}
        tracker.releaseAll()
        precondition(writes.count == beforeFailure + 1 && flags[1] == false)
        failEnable = false

        tracker.withRead(pid: 1) {}
        failReset = true
        tracker.releaseAll()
        precondition(flags[1] == true)
        failReset = false
        tracker.cleanupExpired()
        precondition(flags[1] == false, "failed resets should retry")

        tracker.withRead(pid: 1) {}
        identities[1] = 11
        let beforeReuse = writes.count
        tracker.releaseAll()
        precondition(writes.count == beforeReuse && flags[1] == true,
                     "a reused PID must not reset the replacement process")

        flags[1] = false
        tracker.withRead(pid: 1) {}
        flags[1] = false // Another accessibility client releases the mode.
        let beforeExternalReset = writes.count
        tracker.withRead(pid: 1) {}
        precondition(writes.count == beforeExternalReset, "do not re-enable an externally cleared flag")
        tracker.releaseAll()
        precondition(writes.count == beforeExternalReset + 1 && flags[1] == false)

        var requests = 0
        let counted = EnhancedAccessibility(read: { _ in requests > 0 }, write: { _, enabled in
            requests += enabled ? 1 : -1
            return true
        }, identity: { _ in 1 })
        counted.withRead(pid: 1) {}
        requests += 1 // Another client acquires its own activation later.
        counted.releaseAll()
        precondition(requests == 1, "release only our own request")

        // A receiver may expose false while asynchronously enabling its tree.
        requests = 0
        let pending = EnhancedAccessibility(read: { _ in false }, write: { _, enabled in
            requests += enabled ? 1 : -1
            return true
        }, identity: { _ in 1 })
        pending.withRead(pid: 1) {}
        pending.releaseAll()
        precondition(requests == 0)

        concurrentReaders()
        print("enhanced accessibility ownership tests passed")
    }

    static func concurrentReaders() {
        final class State {
            var enabled = false
            var writes = 0
        }
        let state = State()
        let tracker = EnhancedAccessibility(read: { _ in state.enabled }, write: { _, enabled in
            state.enabled = enabled
            state.writes += 1
            return true
        }, identity: { _ in 1 })
        let entered = DispatchSemaphore(value: 0), finish = DispatchSemaphore(value: 0)
        let group = DispatchGroup()
        for _ in 0..<4 {
            group.enter()
            DispatchQueue.global().async {
                tracker.withRead(pid: 1) {
                    entered.signal()
                    precondition(finish.wait(timeout: .now() + 5) == .success)
                }
                group.leave()
            }
        }
        for _ in 0..<4 { precondition(entered.wait(timeout: .now() + 5) == .success) }
        tracker.releaseAll()
        precondition(state.enabled && state.writes == 1)
        for _ in 0..<4 { finish.signal() }
        precondition(group.wait(timeout: .now() + 5) == .success)
        precondition(!state.enabled && state.writes == 2)
    }
}
