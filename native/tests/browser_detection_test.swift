import Foundation

private final class Element: BrowserAccessibilityElement {
    var role: String
    var title: String?
    var url: String?
    var value: String?
    var nodes: [Element]
    var reads = 0

    init(_ role: String, title: String? = nil, url: String? = nil,
         value: String? = nil, children: [Element] = []) {
        self.role = role
        self.title = title
        self.url = url
        self.value = value
        nodes = children
    }
    func children(limit: Int) -> [Element] {
        reads += 1
        return Array(nodes.prefix(limit))
    }
}

private func process(_ pid: pid_t, _ bundle: String, output: Bool = true,
                     input: Bool = false) -> AudioProcess {
    AudioProcess(objectID: UInt32(pid), pid: pid, bundleID: bundle, name: "helper",
                 isRunningOutput: output, isRunningInput: input)
}
private func tree(_ parents: [pid_t: pid_t]) -> ProcessTree {
    ProcessTree(parents: parents)
}

@main struct BrowserDetectionTests {
    static func main() {
        let document = Element("AXWebArea", url: "https://example.test", children: [
            Element("AXRadioButton", title: "Meet - abc-defg-hij")
        ])
        let tab = Element("AXRadioButton", title: "Meet - aaa-bbbb-ccc - Audio playing")
        let window = Element("AXWindow", title: "Inbox", children: [document, tab])
        let content = BrowserContentReader.read(window, deadline: 1, now: { 0 })
        precondition(content.url == "https://example.test")
        precondition(content.tabTitles == ["Meet - aaa-bbbb-ccc - Audio playing"])
        precondition(document.reads == 0)
        let match = MeetingCatalog.browserWindow(content)
        precondition(match.platform == .meet && match.title == "Meet - aaa-bbbb-ccc" && match.url.isEmpty)

        let expired = BrowserContentReader.read(window, deadline: 0, now: { 0 })
        precondition(expired.title.isEmpty && expired.tabTitles.isEmpty)
        let crowded = Element("AXWindow", children: (0..<400).map { _ in Element("AXGroup") } + [tab])
        let limited = BrowserContentReader.read(crowded, deadline: 1, now: { 0 })
        precondition(limited.tabTitles.isEmpty)
        var deep = tab
        for _ in 0..<13 { deep = Element("AXGroup", children: [deep]) }
        precondition(BrowserContentReader.read(deep, deadline: 1, now: { 0 }).tabTitles.isEmpty)
        let field = Element("AXTextField", value: "meet.google.com/abc-defg-hij")
        precondition(BrowserContentReader.read(field, deadline: 1, now: { 0 }).url == field.value)

        let apps = [
            ApplicationProcess(pid: 100, bundleID: "com.google.chrome", name: "Browser A", isRegular: true),
            ApplicationProcess(pid: 200, bundleID: "com.google.chrome", name: "Browser B", isRegular: true)
        ]
        let processes = [process(101, "com.google.chrome.helper"), process(201, "com.google.chrome.helper")]
        let windows: [pid_t: [BrowserWindowContent]] = [
            100: [BrowserWindowContent(title: "Inbox", url: "https://example.test"), content],
            200: [BrowserWindowContent(title: "Other call", url: "https://teams.microsoft.com/l/meetup-join/1")]
        ]
        let meetings = MeetingDetector.scan(processes: processes, apps: apps,
                                            tree: tree([101: 100, 201: 200]), recorderPID: 999,
                                            browserWindows: { windows[$0] ?? [] }, windowTitle: { _ in nil })
        precondition(meetings.count == 2)
        let first = meetings.first { $0.pid == 100 }!
        let second = meetings.first { $0.pid == 200 }!
        precondition(first.platform == .meet && first.audioPIDs == [101] && first.url.isEmpty)
        precondition(second.platform == .teams && second.audioPIDs == [201])
        precondition(first.appName == "Browser A" && second.appName == "Browser B")

        let registeredHelpers = apps + [ApplicationProcess(
            pid: 101, bundleID: "com.google.chrome.helper", name: "Browser helper", isRegular: false)]
        let withHelper = MeetingDetector.scan(processes: processes, apps: registeredHelpers,
                                             tree: tree([101: 100, 201: 200]), recorderPID: 999,
                                             browserWindows: { windows[$0] ?? [] }, windowTitle: { _ in nil })
        precondition(Set(withHelper.map(\.pid)) == [100, 200])
        let unnamedHelper = MeetingDetector.scan(processes: [process(101, "")], apps: apps,
                                                tree: tree([101: 100]), recorderPID: 999,
                                                browserWindows: { windows[$0] ?? [] }, windowTitle: { _ in nil })
        precondition(unnamedHelper.count == 1 && unnamedHelper[0].pid == 100)

        let noAccess = MeetingDetector.scan(processes: processes, apps: apps,
                                            tree: tree([101: 100, 201: 200]), recorderPID: 999,
                                            browserWindows: { _ in [] }, windowTitle: { _ in nil })
        precondition(noAccess.isEmpty)
        let microphoneOnly = MeetingDetector.scan(
            processes: [process(101, "com.google.chrome.helper", output: false, input: true)], apps: apps,
            tree: tree([101: 100]), recorderPID: 999,
            browserWindows: { _ in [] }, windowTitle: { _ in nil })
        precondition(microphoneOnly.count == 1 && microphoneOnly[0].platform == .genericBrowser)
        precondition(microphoneOnly[0].confidence < MeetingCatalog.recordThreshold)

        let ambiguous = tree([:])
        precondition(AudioAppRegistry.owner(of: processes[0], in: apps, tree: ambiguous) == nil)
        precondition(AudioAppRegistry.owner(of: processes[0], in: [apps[0]], tree: ambiguous)?.pid == 100)
        precondition(AudioAppRegistry.owner(of: process(301, "com.google.chromelike"),
                                           in: [apps[0]], tree: ambiguous) == nil)
        let nestedApp = ApplicationProcess(pid: 300, bundleID: "com.google.chrome.beta",
                                           name: "Browser Beta", isRegular: true)
        precondition(AudioAppRegistry.owner(of: process(301, "com.google.chrome.beta.helper"),
                                           in: apps + [nestedApp], tree: ambiguous)?.pid == 300)
        let cyclic = tree([101: 102, 102: 101])
        precondition(cyclic.owner(of: 101, among: [100]) == nil)

        let recorderAudio = [process(500, "recorder"), process(501, "recorder.helper"),
                             process(502, ""), process(600, "other")]
        let ancestry: [pid_t: pid_t] = [501: 500, 502: 501, 600: 1]
        let excluded = AudioProcessRegistry.recorderProcessObjects(
            in: recorderAudio, recorderPID: 500, tree: tree(ancestry))
        precondition(excluded == [500, 501, 502])
        let laterHelper = recorderAudio + [process(503, "new.helper")]
        precondition(AudioProcessRegistry.recorderProcessObjects(
            in: laterHelper, recorderPID: 500, tree: tree(ancestry.merging([503: 500]) { _, new in new }))
            == [500, 501, 502, 503])

        let selfMeeting = MeetingDetector.scan(
            processes: [process(501, "com.google.chrome.helper")], apps: apps,
            tree: tree([501: 500]), recorderPID: 500,
            browserWindows: { _ in [content] }, windowTitle: { _ in nil })
        precondition(selfMeeting.isEmpty)
        print("browser detection and recorder exclusion tests passed")
    }
}
