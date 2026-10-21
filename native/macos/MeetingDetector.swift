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
    static func scan() -> [DetectedMeeting] {
        let processes = AudioProcessRegistry.all().filter {
            $0.pid != getpid() && AudioProcessRegistry.isAlive($0.pid)
        }

        // Group by application. Helper processes carry the parent's bundle id
        // prefix.
        var byPlatform: [MeetingPlatform: [AudioProcess]] = [:]
        var browserProcesses: [(String, AudioProcess)] = []

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

        // An open app with no audio is not reported.
        let confidence = MeetingCatalog.confidence(
            isBrowser: false, playingAudio: !playing.isEmpty, usingMic: !listening.isEmpty,
            recognised: nil)
        guard confidence > 0 else { return nil }

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
            confidence: confidence
        )
    }

    // MARK: - Browser tabs

    /// A video tab and a call tab are indistinguishable at the process level, so a
    /// browser counts as a meeting only when the URL or title names a known service,
    /// or the microphone is live.
    private static func buildBrowser(
        _ entries: [(String, AudioProcess)]) -> [DetectedMeeting] {
        // Keyed by the browser's bundle-id prefix, which its helpers share.
        var grouped: [String: [AudioProcess]] = [:]
        for (browser, process) in entries {
            grouped[browser, default: []].append(process)
        }

        var meetings: [DetectedMeeting] = []
        for processes in grouped.values {
            let playing = processes.filter(\.isRunningOutput)
            let listening = processes.filter(\.isRunningInput)
            guard !playing.isEmpty || !listening.isEmpty else { continue }

            let owner = ownerPID(for: processes) ?? processes[0].pid
            let url = Accessibility.browserURL(pid: owner) ?? ""
            let page = MeetingCatalog.platform(
                forTitle: Accessibility.focusedWindowTitle(pid: owner) ?? "")
            let matched = MeetingCatalog.platform(forURL: url) ?? page.platform

            // Without a recognised meeting, the microphone is the only usable signal.
            let confidence = MeetingCatalog.confidence(
                isBrowser: true, playingAudio: !playing.isEmpty, usingMic: !listening.isEmpty,
                recognised: matched)
            guard confidence > 0 else { continue }

            let audioPIDs = Array(Set(playing.map(\.pid) + listening.map(\.pid))).sorted()

            meetings.append(DetectedMeeting(
                platform: matched ?? .genericBrowser,
                pid: owner,
                audioPIDs: Array(audioPIDs.prefix(16)),
                appName: processName(of: owner, in: processes),
                title: page.title,
                url: url,
                isUsingMic: !listening.isEmpty,
                isPlayingAudio: !playing.isEmpty,
                confidence: confidence
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
        if let process = processes.first(where: { $0.pid == pid }) {
            return process.name
        }
        if let app = NSWorkspace.shared.runningApplications.first(where: {
            $0.processIdentifier == pid
        }) {
            return app.localizedName
                ?? app.bundleURL?.deletingPathExtension().lastPathComponent
                ?? processes.first?.name
                ?? ""
        }
        return processes.first?.name ?? ""
    }
}

enum AudioAppRegistry {
    /// Regular applications doing audio IO. Helper processes fold into the app
    /// whose bundle id prefixes theirs; daemons with no owning app are left out.
    static func all() -> [AudioProcess] {
        let apps = regularApps()
        var byOwner: [pid_t: (app: NSRunningApplication, processes: [AudioProcess])] = [:]
        for process in AudioProcessRegistry.all() {
            guard let app = owner(of: process, in: apps) else { continue }
            byOwner[app.processIdentifier, default: (app, [])].processes.append(process)
        }
        return byOwner.values.map { app, processes in
            AudioProcess(
                objectID: processes[0].objectID,
                pid: app.processIdentifier,
                bundleID: app.bundleIdentifier ?? processes[0].bundleID,
                name: app.localizedName ?? processes[0].name,
                isRunningOutput: processes.contains(where: \.isRunningOutput),
                isRunningInput: processes.contains(where: \.isRunningInput)
            )
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// `pids` plus the audio processes of any app among them, so tapping an app
    /// from `all()` reaches audio its helper processes play.
    static func includingHelpers(_ pids: [pid_t]) -> [pid_t] {
        let apps = regularApps()
        let helpers = AudioProcessRegistry.all().compactMap { process -> pid_t? in
            guard let app = owner(of: process, in: apps),
                  pids.contains(app.processIdentifier) else { return nil }
            return process.pid
        }
        return Array(Set(pids + helpers)).sorted()
    }

    private static func regularApps() -> [NSRunningApplication] {
        NSWorkspace.shared.runningApplications.filter {
            $0.activationPolicy == .regular && $0.processIdentifier != getpid()
        }
    }

    private static func owner(of process: AudioProcess,
                              in apps: [NSRunningApplication]) -> NSRunningApplication? {
        if let direct = apps.first(where: { $0.processIdentifier == process.pid }) {
            return direct
        }
        let bundleID = process.bundleID.lowercased()
        guard !bundleID.isEmpty else { return nil }
        // The longest matching prefix wins, so `com.microsoft.teams2` beats `com.microsoft`.
        return apps
            .filter { app in
                guard let id = app.bundleIdentifier?.lowercased() else { return false }
                return bundleID.hasPrefix(id)
            }
            .max { ($0.bundleIdentifier?.count ?? 0) < ($1.bundleIdentifier?.count ?? 0) }
    }
}
