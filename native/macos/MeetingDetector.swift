import AppKit
import CoreAudio
import Foundation

/// A detected meeting, before it is flattened into the C struct.
struct DetectedMeeting {
    var platform: MeetingPlatform
    var pid: pid_t
    var audioPIDs: [pid_t]
    var appName: String
    var title: String
    var url: String
    var isUsingMic: Bool
    var isPlayingAudio: Bool
    var confidence: Int32
}

/// Decides whether the user is in a meeting, and which processes to record.
///
/// Identity and audio activity come from property reads that need no permission.
/// Accessibility only adds a title or URL, so full confidence is reachable without
/// it.
enum MeetingDetector {
    /// Confidence weights. A known app being open is not a meeting on its own.
    private enum Score {
        static let knownApp: Int32 = 40
        static let playingAudio: Int32 = 30
        static let usingMic: Int32 = 25
        static let recognisedURL: Int32 = 5
        /// Below this, callers should not start recording.
        static let recordThreshold: Int32 = 70
    }

    static func scan() -> [DetectedMeeting] {
        let processes = AudioProcessRegistry.all().filter {
            $0.pid != getpid() && AudioProcessRegistry.isAlive($0.pid)
        }

        // Group by application. Helper processes carry the parent's bundle id
        // prefix.
        var byPlatform: [MeetingPlatform: [AudioProcess]] = [:]
        var browserProcesses: [(BrowserApp, AudioProcess)] = []

        for process in processes {
            if let platform = MeetingCatalog.nativePlatform(forBundleID: process.bundleID) {
                byPlatform[platform, default: []].append(process)
            } else if let browser = MeetingCatalog.browser(forBundleID: process.bundleID) {
                browserProcesses.append((browser, process))
            }
        }

        var meetings: [DetectedMeeting] = []
        for (platform, group) in byPlatform {
            if let meeting = buildNative(platform: platform, processes: group) {
                meetings.append(meeting)
            }
        }
        meetings.append(contentsOf: buildBrowser(browserProcesses))

        return meetings.sorted { $0.confidence > $1.confidence }
    }

    // MARK: - Native clients

    private static func buildNative(platform: MeetingPlatform,
                                    processes: [AudioProcess]) -> DetectedMeeting? {
        let playing = processes.filter(\.isRunningOutput)
        let listening = processes.filter(\.isRunningInput)

        var confidence = Score.knownApp
        if !playing.isEmpty { confidence += Score.playingAudio }
        if !listening.isEmpty { confidence += Score.usingMic }

        // An open app with no audio is not reported.
        guard !playing.isEmpty || !listening.isEmpty else { return nil }

        // `pid` is the user-visible app; `audioPIDs` is what carries the audio.
        let owner = ownerPID(for: processes) ?? processes[0].pid
        let audioPIDs = Array(Set(playing.map(\.pid) + listening.map(\.pid))).sorted()

        let title = Accessibility.focusedWindowTitle(pid: owner) ?? ""

        return DetectedMeeting(
            platform: platform,
            pid: owner,
            audioPIDs: Array(audioPIDs.prefix(16)),
            appName: processName(of: owner, in: processes),
            title: title,
            url: "",
            isUsingMic: !listening.isEmpty,
            isPlayingAudio: !playing.isEmpty,
            confidence: min(confidence, 100)
        )
    }

    // MARK: - Browser tabs

    /// A video tab and a call tab are indistinguishable at the process level, so a
    /// browser counts as a meeting only when the URL or title names a known service,
    /// or the microphone is live.
    private static func buildBrowser(
        _ entries: [(BrowserApp, AudioProcess)]) -> [DetectedMeeting] {
        var grouped: [String: (browser: BrowserApp, processes: [AudioProcess])] = [:]
        for (browser, process) in entries {
            grouped[browser.bundlePrefix, default: (browser, [])].processes.append(process)
        }

        var meetings: [DetectedMeeting] = []
        for (_, entry) in grouped {
            let playing = entry.processes.filter(\.isRunningOutput)
            let listening = entry.processes.filter(\.isRunningInput)
            guard !playing.isEmpty || !listening.isEmpty else { continue }

            let owner = ownerPID(for: entry.processes) ?? entry.processes[0].pid
            let url = Accessibility.browserURL(pid: owner) ?? ""
            let title = Accessibility.focusedWindowTitle(pid: owner) ?? ""

            let matched = MeetingCatalog.platform(forURLOrTitle: url)
                ?? MeetingCatalog.platform(forURLOrTitle: title)

            // Without a URL match, the microphone is the only usable signal.
            guard matched != nil || !listening.isEmpty else { continue }

            var confidence = Score.knownApp
            if !playing.isEmpty { confidence += Score.playingAudio }
            if !listening.isEmpty { confidence += Score.usingMic }
            if matched != nil { confidence += Score.recognisedURL }

            let audioPIDs = Array(Set(playing.map(\.pid) + listening.map(\.pid))).sorted()

            meetings.append(DetectedMeeting(
                platform: matched ?? .genericBrowser,
                pid: owner,
                audioPIDs: Array(audioPIDs.prefix(16)),
                appName: processName(of: owner, in: entry.processes),
                title: title,
                url: url,
                isUsingMic: !listening.isEmpty,
                isPlayingAudio: !playing.isEmpty,
                confidence: min(confidence, 100)
            ))
        }
        return meetings
    }

    // MARK: - Helpers

    /// The user-visible process for a group of audio processes. `NSWorkspace` lists
    /// only real applications, which filters out helpers.
    private static func ownerPID(for processes: [AudioProcess]) -> pid_t? {
        let apps = NSWorkspace.shared.runningApplications
        let appPIDs = Set(apps.map(\.processIdentifier))
        if let direct = processes.first(where: { appPIDs.contains($0.pid) }) {
            return direct.pid
        }
        // Otherwise match the helper's bundle id prefix against a real app.
        guard let bundleID = processes.first?.bundleID.lowercased(),
              !bundleID.isEmpty else { return nil }
        return apps.first { app in
            guard let id = app.bundleIdentifier?.lowercased() else { return false }
            return bundleID.hasPrefix(id) || id.hasPrefix(bundleID)
        }?.processIdentifier
    }

    /// Executable name as reported by the kernel.
    private static func processName(of pid: pid_t,
                                    in processes: [AudioProcess]) -> String {
        processes.first { $0.pid == pid }?.name ?? processes.first?.name ?? ""
    }

    static var recordThreshold: Int32 { Score.recordThreshold }
}
