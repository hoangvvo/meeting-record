import Foundation

private final class Element: BrowserAccessibilityElement {
    var role: String
    var title: String?
    var url: String?
    var value: String?
    var identifier: String?
    var subrole: String?
    var isHidden: Bool
    var nodes: [Element]
    var reads = 0

    init(_ role: String, title: String? = nil, url: String? = nil,
         value: String? = nil, identifier: String? = nil, hidden: Bool = false,
         subrole: String? = nil, children: [Element] = []) {
        self.role = role
        self.title = title
        self.url = url
        self.value = value
        self.identifier = identifier
        self.subrole = subrole
        self.isHidden = hidden
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

        func page(_ url: String, title: String = "Meeting", controls: [Element]) -> BrowserWindowContent {
            BrowserContentReader.read(Element("AXWindow", title: title, children: [
                Element("AXWebArea", url: url, children: controls)
            ]), deadline: 1, now: { 0 })
        }
        let teamsCall = page("https://teams.microsoft.com/v2/", title: "Daily sync | Microsoft Teams",
                             controls: [Element("AXButton", title: "Localized leave", identifier: "hangup-button")])
        let teamsPreview = page("https://teams.microsoft.com/v2/", controls: [
            Element("AXButton", title: "Localized join", identifier: "prejoin-join-button")
        ])
        let meetCall = page("https://meet.google.com/abc-defg-hij", title: "Meet - abc-defg-hij", controls: [
            Element("AXButton", title: "Leave call")
        ])
        var meetPreview = page("https://meet.google.com/abc-defg-hij", title: "Meet - abc-defg-hij", controls: [
            Element("AXButton", title: "Join now")
        ])
        // An active preview cannot borrow positive evidence from a background tab title.
        meetPreview.tabTitles = ["Meet - aaa-bbbb-ccc"]
        let meetEnded = page("https://meet.google.com/abc-defg-hij", controls: [Element("AXButton", title: "Rejoin")])
        precondition(MeetingCatalog.browserWindow(teamsCall).platform == .teams)
        precondition(MeetingCatalog.browserWindow(teamsCall).callState == .joined)
        precondition(MeetingCatalog.browserWindow(teamsPreview).callState == .notJoined)
        precondition(MeetingCatalog.browserWindow(meetPreview).callState == .notJoined)
        precondition(MeetingCatalog.browserWindow(meetEnded).callState == .notJoined)

        let quotedControls = page("https://meet.google.com/abc-defg-hij", controls: [
            Element("AXStaticText", title: "Leave call"),
            Element("AXButton", title: "Leave call", hidden: true),
            Element("AXTextField", value: "Leave call", children: [Element("AXButton", title: "Leave call")])
        ])
        precondition(MeetingCatalog.browserWindow(quotedControls).callState == .unknown)
        let unrelated = Element("AXWebArea", url: "https://example.test", children: [
            Element("AXButton", title: "Leave call", identifier: "hangup-button")
        ])
        let unrelatedContent = BrowserContentReader.read(
            Element("AXWindow", title: "Meet - abc-defg-hij", children: [unrelated]), deadline: 1, now: { 0 })
        precondition(unrelated.reads == 0 && unrelatedContent.callSignals == 0)
        let embeddedOtherSite = page("https://teams.microsoft.com/v2/", controls: [
            Element("AXWebArea", url: "https://example.test/", children: [Element("AXButton", identifier: "hangup-button")])
        ])
        precondition(embeddedOtherSite.callSignals == 0, "an unrelated iframe cannot supply call controls")
        let lobby = page("https://teams.microsoft.com/v2/", controls: [
            Element("AXGroup", subrole: "AXApplicationStatus", children: [
                Element("AXStaticText", title: "We've let people know you're waiting.")
            ])
        ])
        precondition(MeetingCatalog.browserWindow(lobby).callState == .notJoined)
        let quotedLobby = page("https://teams.microsoft.com/v2/", controls: [
            Element("AXStaticText", title: "We've let people know you're waiting."),
            Element("AXButton", identifier: "hangup-button")
        ])
        precondition(MeetingCatalog.browserWindow(quotedLobby).callState == .joined)

        var callDepth = Element("AXButton", title: "Leave call")
        for _ in 0..<25 { callDepth = Element("AXGroup", children: [callDepth]) }
        precondition(page("https://meet.google.com/abc-defg-hij", controls: [callDepth]).callSignals == 0)
        let callCrowd = (0..<1300).map { _ in Element("AXGroup") } + [Element("AXButton", title: "Leave call")]
        precondition(page("https://meet.google.com/abc-defg-hij", controls: callCrowd).callSignals == 0)

        let apps = [
            ApplicationProcess(pid: 100, bundleID: "com.google.chrome", name: "Browser A", isRegular: true),
            ApplicationProcess(pid: 200, bundleID: "com.google.chrome", name: "Browser B", isRegular: true)
        ]
        let processes = [process(101, "com.google.chrome.helper", input: true),
                         process(201, "com.google.chrome.helper", input: true)]
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
        let unnamedHelper = MeetingDetector.scan(processes: [process(101, "", input: true)], apps: apps,
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
        precondition(microphoneOnly.isEmpty)

        let playbackOnly = MeetingDetector.scan(
            processes: [process(101, "com.google.chrome.helper")], apps: apps,
            tree: tree([101: 100]), recorderPID: 999,
            browserWindows: { windows[$0] ?? [] }, windowTitle: { _ in nil })
        precondition(playbackOnly.isEmpty, "a stale call tab plus video playback is not a call")
        let teamsChat = MeetingDetector.scan(
            processes: processes, apps: apps, tree: tree([101: 100, 201: 200]), recorderPID: 999,
            browserWindows: { _ in [BrowserWindowContent(
                title: "Chat | Debt | Microsoft Teams", url: "https://teams.microsoft.com/v2/")] },
            windowTitle: { _ in nil })
        precondition(teamsChat.isEmpty, "Teams chat is not a browser meeting, even with mic activity")

        func browserScan(_ pages: [BrowserWindowContent], audio: [AudioProcess] = []) -> [DetectedMeeting] {
            MeetingDetector.scan(processes: audio, apps: [apps[0]], tree: tree([101: 100]),
                                 recorderPID: 999, browserWindows: { _ in pages }, windowTitle: { _ in nil })
        }
        for callPage in [teamsCall, meetCall] {
            let silent = browserScan([callPage])
            precondition(silent.count == 1 && silent[0].confidence >= MeetingCatalog.recordThreshold)
            precondition(!silent[0].isUsingMic && !silent[0].isPlayingAudio && silent[0].audioPIDs == [100])
            let listenOnly = browserScan([callPage], audio: [process(101, "com.google.chrome.helper")])
            precondition(listenOnly.count == 1 && listenOnly[0].confidence == 95)
            precondition(listenOnly[0].audioPIDs == [101])
        }
        for preview in [teamsPreview, meetPreview, meetEnded, lobby] {
            precondition(browserScan([preview], audio: [process(101, "com.google.chrome.helper", input: true)]).isEmpty)
        }
        let anotherWindow = browserScan([teamsPreview, meetCall], audio: [process(101, "com.google.chrome.helper", input: true)])
        precondition(anotherWindow.count == 1 && anotherWindow[0].platform == .meet)
        precondition(anotherWindow[0].title == "Meet - abc-defg-hij")
        let otherBrowserCalls: [(String, MeetingPlatform, [Element])] = [
            ("https://zoom.us/wc/123/join", .zoom, [Element("AXButton", title: "Leave meeting")]),
            ("https://company.webex.com/meet/person", .webex, [Element("AXButton", title: "Leave meeting")]),
            ("https://app.slack.com/huddle/T01/C02", .slack, [Element("AXButton", title: "Leave huddle")]),
            ("https://discord.com/channels/1/2", .discord, [Element("AXButton", title: "Disconnect"), Element("AXStaticText", title: "Voice Connected")]),
            ("https://whereby.com/room", .genericBrowser, [Element("AXButton", title: "Leave call")]),
        ]
        for (url, platform, controls) in otherBrowserCalls {
            let call = browserScan([page(url, controls: controls)])
            precondition(call.count == 1 && call[0].platform == platform && call[0].confidence == 90)
        }

        // The same playback rule covers every native client, including audio helpers.
        let nativeApps: [(String, MeetingPlatform)] = [
            ("us.zoom.xos", .zoom), ("com.microsoft.teams2", .teams),
            ("com.cisco.webexmeetingsapp", .webex), ("Cisco-Systems.Spark", .webex),
            ("com.tinyspeck.slackmacgap", .slack), ("com.hnc.Discord", .discord)
        ]
        for (bundle, platform) in nativeApps {
            let app = ApplicationProcess(pid: 300, bundleID: bundle, name: "Native", isRegular: true)
            func scan(_ audio: [AudioProcess]) -> [DetectedMeeting] {
                MeetingDetector.scan(processes: audio, apps: [app], tree: tree([301: 300, 302: 300]),
                                     recorderPID: 999, browserWindows: { _ in [] },
                                     windowTitle: { _ in "Chat | Debt | Microsoft Teams" })
            }
            precondition(scan([process(301, bundle + ".helper")]).isEmpty)
            precondition(scan([process(301, bundle + ".helper", output: false)]).isEmpty)
            let call = scan([process(301, bundle + ".helper"),
                             process(302, bundle + ".helper", output: false, input: true)])
            precondition(call.count == 1 && call[0].platform == platform && call[0].pid == 300)
            precondition(call[0].audioPIDs == [301, 302] && call[0].confidence == 95)
        }

        let nativeControls: [(String, MeetingPlatform, [Element])] = [
            ("com.microsoft.teams2", .teams, [Element("AXButton", identifier: "hangup-button")]),
            ("us.zoom.xos", .zoom, [Element("AXMenuItem", title: "Leave Meeting")]),
            ("com.cisco.webexmeetingsapp", .webex, [Element("AXButton", title: "Leave meeting")]),
            ("com.tinyspeck.slackmacgap", .slack, [Element("AXButton", title: "Leave huddle")]),
            ("com.hnc.Discord", .discord, [Element("AXButton", title: "Disconnect"), Element("AXStaticText", title: "Voice Connected")]),
        ]
        for (bundle, platform, controls) in nativeControls {
            let nativePage = BrowserContentReader.read(Element("AXWindow", title: "Confirmed call", children: controls),
                                                       deadline: 1, now: { 0 }, nativePlatform: platform)
            let app = ApplicationProcess(pid: 300, bundleID: bundle, name: "Native", isRegular: true)
            let silent = MeetingDetector.scan(
                processes: [], apps: [app], tree: tree([:]), recorderPID: 999,
                browserWindows: { _ in [] }, windowTitle: { _ in "Chat" }, nativeWindows: { _, _ in [nativePage] })
            precondition(silent.count == 1 && silent[0].platform == platform && silent[0].confidence == 90)
            precondition(silent[0].pid == 300 && silent[0].audioPIDs == [300] && silent[0].title == "Confirmed call")
        }
        let nativePreview = BrowserContentReader.read(
            Element("AXWindow", children: [Element("AXButton", identifier: "prejoin-join-button")]),
            deadline: 1, now: { 0 }, nativePlatform: .teams)
        let previewApp = ApplicationProcess(pid: 300, bundleID: "com.microsoft.teams2", name: "Teams", isRegular: true)
        let nativePreviewScan = MeetingDetector.scan(
            processes: [process(301, "com.microsoft.teams2.modulehost", input: true)], apps: [previewApp],
            tree: tree([301: 300]), recorderPID: 999, browserWindows: { _ in [] }, windowTitle: { _ in nil },
            nativeWindows: { _, _ in [nativePreview] })
        precondition(nativePreviewScan.isEmpty, "a microphone preview must not produce a recordable meeting")

        let zoomWaiting = BrowserContentReader.read(Element("AXWindow", children: [
            Element("AXButton", title: "Leave meeting"),
            Element("AXHeading", title: "Please wait for the host to start this meeting.")
        ]), deadline: 1, now: { 0 }, nativePlatform: .zoom)
        precondition(MeetingCatalog.browserWindow(zoomWaiting).callState == .notJoined)
        let zoomAudioDisconnected = BrowserContentReader.read(Element("AXWindow", children: [
            Element("AXMenuItem", title: "Leave Meeting"), Element("AXButton", title: "Join Audio")
        ]), deadline: 1, now: { 0 }, nativePlatform: .zoom)
        precondition(MeetingCatalog.browserWindow(zoomAudioDisconnected).callState == .joined)

        let selfApp = ApplicationProcess(pid: 999, bundleID: "com.google.chrome", name: "Recorder", isRegular: true)
        let selfSeed = MeetingDetector.scan(processes: [], apps: [selfApp], tree: tree([:]), recorderPID: 999,
                                            browserWindows: { _ in [meetCall] }, windowTitle: { _ in nil })
        precondition(selfSeed.isEmpty)

        let nativeInstances = [
            ApplicationProcess(pid: 300, bundleID: "com.microsoft.teams2", name: "Teams A", isRegular: true),
            ApplicationProcess(pid: 400, bundleID: "com.microsoft.teams2", name: "Teams B", isRegular: true)
        ]
        let separateActivity = MeetingDetector.scan(
            processes: [process(301, "com.microsoft.teams2.modulehost"),
                        process(401, "com.microsoft.teams2.modulehost", output: false, input: true)],
            apps: nativeInstances, tree: tree([301: 300, 401: 400]), recorderPID: 999,
            browserWindows: { _ in [] }, windowTitle: { "window \($0)" })
        precondition(separateActivity.count == 1 && separateActivity[0].pid == 400)
        precondition(separateActivity[0].audioPIDs == [401] && separateActivity[0].title == "window 400")
        precondition(separateActivity[0].confidence < MeetingCatalog.recordThreshold,
                     "one instance's playback must not raise another instance's mic-only confidence")

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
