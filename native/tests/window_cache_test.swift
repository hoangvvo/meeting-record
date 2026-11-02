import Foundation

@main struct WindowCacheTests {
    static func main() {
        let apps = [
            ApplicationProcess(pid: 100, bundleID: "com.google.chrome", name: "Chrome", isRegular: true),
            ApplicationProcess(pid: 200, bundleID: "com.tinyspeck.slackmacgap", name: "Slack", isRegular: true),
        ]
        var time: TimeInterval = 0
        var identities: [pid_t: UInt64] = [100: 1, 200: 1]
        let cache = MeetingWindowCache(identity: { identities[$0] }, now: { time })
        var reads: [pid_t: Int] = [:]
        var browser = BrowserWindowContent(title: "Inbox", url: "https://example.com")
        var native = BrowserWindowContent(title: "Slack", callPlatform: .slack)
        @discardableResult
        func scan(cached: Bool = true, foreground: pid_t? = nil,
                  audio: [AudioProcess] = [], refreshing: Set<pid_t> = []) -> [DetectedMeeting] {
            MeetingDetector.scan(processes: audio, apps: apps,
                tree: ProcessTree(parents: [101: 100, 201: 200]), recorderPID: 999,
                browserWindows: { pid in reads[pid, default: 0] += 1; return [browser] },
                windowTitle: { _ in nil },
                nativeWindows: { pid, _ in reads[pid, default: 0] += 1; return [native] },
                cache: cached ? cache : nil, foregroundPID: foreground, forceFreshPIDs: refreshing)
        }
        precondition(scan().isEmpty && reads == [100: 1, 200: 1])
        time = 5
        scan()
        precondition(reads == [100: 1, 200: 1], "background idle apps should reuse UI snapshots")
        time = 6
        scan(foreground: 100)
        precondition(reads == [100: 2, 200: 1])
        let slackAudio = AudioProcess(objectID: 1, pid: 201, bundleID: "com.tinyspeck.slackmacgap.helper",
            name: "Slack helper", isRunningOutput: true, isRunningInput: true)
        time = 7
        precondition(scan(audio: [slackAudio]).first?.platform == .slack)
        precondition(reads == [100: 2, 200: 2], "audio-active apps must bypass idle caching")

        browser = BrowserWindowContent(title: "Call", url: "https://meet.google.com/abc-defg-hij")
        time = 8
        scan(foreground: 100)
        time = 9
        scan()
        precondition(reads[100] == 4, "a call URL stays fresh even before joined controls appear")
        browser.callPlatform = .meet
        browser.callSignals = UInt32(MREC_CALL_SIGNAL_JOINED)
        time = 10
        let silentCall = scan()
        precondition(silentCall.count == 1 && silentCall[0].confidence == 90)
        browser.callSignals = UInt32(MREC_CALL_SIGNAL_NOT_JOINED)
        time = 11
        precondition(scan().isEmpty && reads[100] == 6,
                     "cached joined UI must not keep an ended call alive")

        browser = BrowserWindowContent(title: "Inbox", url: "https://example.com")
        time = 12
        scan()
        let idleReads = reads[100]
        time = 41
        scan()
        precondition(reads[100] == idleReads)
        time = 42
        scan()
        precondition(reads[100] == idleReads! + 1)
        scan(cached: false)
        precondition(reads[100] == idleReads! + 2, "direct scans are always fresh")
        identities[100] = 2
        scan()
        precondition(reads[100] == idleReads! + 3, "a reused PID must not inherit a snapshot")

        native.callSignals = UInt32(MREC_CALL_SIGNAL_JOINED)
        scan(foreground: 200)
        let nativeReads = reads[200]!
        time = 43
        precondition(scan().first?.platform == .slack)
        precondition(reads[200] == nativeReads + 1, "silent native calls must also stay fresh")
        cache.removeAll()
        let beforeStop = reads[100]!
        scan()
        precondition(reads[100] == beforeStop + 1)

        browser = BrowserWindowContent(title: "Call", url: "https://meet.google.com/abc-defg-hij",
            callPlatform: .meet, callSignals: UInt32(MREC_CALL_SIGNAL_JOINED))
        precondition(scan(foreground: 100).contains { $0.pid == 100 })
        browser = BrowserWindowContent() // A transient failed/partial read.
        precondition(!scan().contains { $0.pid == 100 })
        browser = BrowserWindowContent(title: "Call", callPlatform: .meet,
            callSignals: UInt32(MREC_CALL_SIGNAL_JOINED))
        precondition(scan(refreshing: [100]).contains { $0.pid == 100 },
                     "tracked calls must retry fresh UI during the end-delay window")
        print("watcher UI cache tests passed")
    }
}
