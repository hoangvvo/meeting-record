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

/// Detect meetings from app identity and audio activity; accessibility adds titles and URLs.
enum MeetingDetector {
    static func scan() -> [DetectedMeeting] {
        let processes = AudioProcessRegistry.all()
        return scan(processes: processes, apps: AudioAppRegistry.applications(),
             tree: ProcessTree.read(ancestorsOf: processes.map(\.pid)), recorderPID: getpid(),
             browserWindows: Accessibility.browserWindows,
             windowTitle: Accessibility.focusedWindowTitle)
    }

    static func scan(processes allProcesses: [AudioProcess], apps: [ApplicationProcess],
                     tree: ProcessTree, recorderPID: pid_t,
                     browserWindows: (pid_t) -> [BrowserWindowContent],
                     windowTitle: (pid_t) -> String?) -> [DetectedMeeting] {
        let processes = allProcesses.filter {
            tree.owner(of: $0.pid, among: [recorderPID]) == nil
        }

        var byPlatform: [MeetingPlatform: [AudioProcess]] = [:]
        var browserProcesses: [AudioProcess] = []

        for process in processes {
            let bundleID = AudioAppRegistry.owner(of: process, in: apps, tree: tree)?.bundleID
                ?? process.bundleID
            if let platform = MeetingCatalog.nativePlatform(forBundleID: bundleID) {
                byPlatform[platform, default: []].append(process)
            } else if MeetingCatalog.browser(forBundleID: bundleID) != nil {
                browserProcesses.append(process)
            }
        }

        var meetings: [DetectedMeeting] = []
        for (platform, group) in byPlatform {
            if let meeting = buildNative(platform: platform, processes: group, apps: apps,
                                         tree: tree, windowTitle: windowTitle) {
                meetings.append(meeting)
            }
        }
        meetings.append(contentsOf: buildBrowser(browserProcesses, apps: apps, tree: tree,
                                                windows: browserWindows))

        return meetings.sorted { $0.confidence > $1.confidence }
    }

    // MARK: - Native clients

    private static func buildNative(platform: MeetingPlatform,
                                    processes: [AudioProcess], apps: [ApplicationProcess],
                                    tree: ProcessTree,
                                    windowTitle: (pid_t) -> String?) -> DetectedMeeting? {
        let playing = processes.filter(\.isRunningOutput)
        let listening = processes.filter(\.isRunningInput)

        // An open app with no audio is not reported.
        let confidence = MeetingCatalog.confidence(
            isBrowser: false, playingAudio: !playing.isEmpty, usingMic: !listening.isEmpty,
            recognised: nil)
        guard confidence > 0 else { return nil }

        // `pid` is the user-visible app; `audioPIDs` is what carries the audio.
        let owner = processes.compactMap { AudioAppRegistry.owner(of: $0, in: apps, tree: tree) }
            .map(\.pid).min() ?? processes[0].pid
        let audioPIDs = Array(Set(playing.map(\.pid) + listening.map(\.pid))).sorted()

        let title = windowTitle(owner) ?? ""

        return DetectedMeeting(
            platform: platform,
            pid: owner,
            audioPIDs: Array(audioPIDs.prefix(16)),
            appName: processName(of: owner, in: processes, apps: apps),
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
        _ entries: [AudioProcess], apps: [ApplicationProcess],
        tree: ProcessTree, windows: (pid_t) -> [BrowserWindowContent]
    ) -> [DetectedMeeting] {
        var grouped: [pid_t: [AudioProcess]] = [:]
        for process in entries {
            let owner = AudioAppRegistry.owner(of: process, in: apps, tree: tree)?.pid ?? process.pid
            grouped[owner, default: []].append(process)
        }

        var meetings: [DetectedMeeting] = []
        for (owner, processes) in grouped {
            let playing = processes.filter(\.isRunningOutput)
            let listening = processes.filter(\.isRunningInput)
            guard !playing.isEmpty || !listening.isEmpty else { continue }

            let pages = windows(owner).map(MeetingCatalog.browserWindow)
            let page = pages.first(where: { $0.platform != nil })
                ?? pages.first ?? (platform: nil, title: "", url: "")
            let confidence = MeetingCatalog.confidence(
                isBrowser: true, playingAudio: !playing.isEmpty, usingMic: !listening.isEmpty,
                recognised: page.platform)
            guard confidence > 0 else { continue }
            let audioPIDs = Array(Set(playing.map(\.pid) + listening.map(\.pid))).sorted()
            meetings.append(DetectedMeeting(
                platform: page.platform ?? .genericBrowser,
                pid: owner, audioPIDs: Array(audioPIDs.prefix(16)),
                appName: processName(of: owner, in: processes, apps: apps),
                title: page.title, url: page.url,
                isUsingMic: !listening.isEmpty, isPlayingAudio: !playing.isEmpty,
                confidence: confidence
            ))
        }
        return meetings
    }

    // MARK: - Helpers

    /// Executable name as reported by the kernel.
    private static func processName(of pid: pid_t,
                                    in processes: [AudioProcess], apps: [ApplicationProcess]) -> String {
        if let process = processes.first(where: { $0.pid == pid }) {
            return process.name
        }
        if let app = apps.first(where: { $0.pid == pid }) { return app.name }
        return processes.first?.name ?? ""
    }
}

enum AudioAppRegistry {
    static func applications() -> [ApplicationProcess] {
        NSWorkspace.shared.runningApplications.map {
            ApplicationProcess(pid: $0.processIdentifier, bundleID: $0.bundleIdentifier ?? "",
                               name: $0.localizedName ?? "", isRegular: $0.activationPolicy == .regular)
        }
    }

    static func all() -> [AudioProcess] {
        let apps = applications().filter { $0.isRegular && $0.pid != getpid() }
        let processes = AudioProcessRegistry.all()
        let tree = ProcessTree.read(ancestorsOf: processes.map(\.pid))
        var byOwner: [pid_t: (app: ApplicationProcess, processes: [AudioProcess])] = [:]
        for process in processes {
            guard let app = owner(of: process, in: apps, tree: tree) else { continue }
            byOwner[app.pid, default: (app, [])].processes.append(process)
        }
        return byOwner.values.map { app, processes in
            AudioProcess(objectID: processes[0].objectID, pid: app.pid,
                         bundleID: app.bundleID, name: app.name,
                         isRunningOutput: processes.contains(where: \.isRunningOutput),
                         isRunningInput: processes.contains(where: \.isRunningInput))
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    static func includingHelpers(_ pids: [pid_t]) -> [pid_t] {
        let apps = applications().filter { $0.isRegular }
        let processes = AudioProcessRegistry.all()
        let tree = ProcessTree.read(ancestorsOf: processes.map(\.pid))
        let helpers = processes.compactMap { process -> pid_t? in
            guard let app = owner(of: process, in: apps, tree: tree),
                  pids.contains(app.pid) else { return nil }
            return process.pid
        }
        return Array(Set(pids + helpers)).sorted()
    }

    static func owner(of process: AudioProcess, in apps: [ApplicationProcess],
                      tree: ProcessTree) -> ApplicationProcess? {
        let regular = Set(apps.filter(\.isRegular).map(\.pid))
        let owner = tree.owner(of: process.pid, among: regular)
            ?? tree.owner(of: process.pid, among: Set(apps.map(\.pid)))
        if let pid = owner {
            return apps.first { $0.pid == pid }
        }
        let bundleID = process.bundleID.lowercased()
        guard !bundleID.isEmpty else { return nil }
        let matches = apps.filter {
            let candidate = $0.bundleID.lowercased()
            return !candidate.isEmpty && (bundleID == candidate || bundleID.hasPrefix(candidate + "."))
        }
        let longest = matches.map { $0.bundleID.count }.max()
        let best = matches.filter { $0.bundleID.count == longest }
        // A bundle ID alone cannot distinguish separately launched instances.
        return best.count == 1 ? best[0] : nil
    }
}
