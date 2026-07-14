import Foundation

/// Which applications count as conferencing apps, and how to recognise a meeting
/// inside a browser.
///
/// Bundle-id matching is deliberately prefix-based: every one of these ships
/// helper processes with suffixed identifiers (`com.google.Chrome.helper`,
/// `us.zoom.xos.cpthost`, `com.microsoft.teams2.helper`) and the audio almost
/// always comes from a helper rather than the main process.
enum MeetingPlatform: Int32 {
    case unknown = 0
    case zoom = 1
    case teams = 2
    case meet = 3
    case webex = 4
    case slack = 5
    case discord = 6
    case genericBrowser = 7

    /// Stable machine identifier. Safe to compare, persist, and use in filenames.
    /// Never change these values: consumers branch on them.
    var id: String {
        switch self {
        case .unknown: return "unknown"
        case .zoom: return "zoom"
        case .teams: return "teams"
        case .meet: return "meet"
        case .webex: return "webex"
        case .slack: return "slack"
        case .discord: return "discord"
        case .genericBrowser: return "browser"
        }
    }

}

struct NativeApp {
    let platform: MeetingPlatform
    /// Matched as a prefix against the process bundle id, lowercased.
    let bundlePrefixes: [String]
}

struct BrowserApp {
    let bundlePrefix: String
}

enum MeetingCatalog {
    /// Native conferencing clients.
    static let nativeApps: [NativeApp] = [
        NativeApp(platform: .zoom, bundlePrefixes: ["us.zoom."]),
        // teams2 is the current client; the original `com.microsoft.teams` still
        // exists on older installs.
        NativeApp(platform: .teams, bundlePrefixes: ["com.microsoft.teams"]),
        NativeApp(platform: .webex, bundlePrefixes: ["com.cisco.webex",
                                                     "com.webex.meetingmanager"]),
        NativeApp(platform: .slack, bundlePrefixes: ["com.tinyspeck.slackmacgap"]),
        NativeApp(platform: .discord, bundlePrefixes: ["com.hnc.discord"]),
    ]

    /// Browsers whose tabs may host a call. Audio-capable helper processes of
    /// these are candidates, but only count as a meeting once a URL or title
    /// matches, or the mic is live. No label is stored: the process name comes
    /// from the OS, and naming things is the consumer's concern.
    static let browsers: [BrowserApp] = [
        BrowserApp(bundlePrefix: "com.google.chrome"),
        BrowserApp(bundlePrefix: "com.apple.safari"),
        BrowserApp(bundlePrefix: "com.microsoft.edgemac"),
        BrowserApp(bundlePrefix: "org.mozilla.firefox"),
        BrowserApp(bundlePrefix: "company.thebrowser.browser"),
        BrowserApp(bundlePrefix: "com.brave.browser"),
        BrowserApp(bundlePrefix: "com.vivaldi.vivaldi"),
    ]

    /// URL host/path fragments that identify a live call, mapped to a platform.
    /// Checked against a lowercased URL *or* window title, since a title is often
    /// all the Accessibility tree exposes.
    static let urlPatterns: [(pattern: String, platform: MeetingPlatform)] = [
        ("meet.google.com", .meet),
        ("teams.microsoft.com", .teams),
        ("teams.live.com", .teams),
        ("zoom.us/j/", .zoom),
        ("zoom.us/wc/", .zoom),
        ("/webex.com/meet", .webex),
        ("webex.com/wbxmjs", .webex),
        ("app.slack.com/huddle", .slack),
        ("discord.com/channels", .discord),
        ("whereby.com/", .genericBrowser),
        ("meet.jit.si", .genericBrowser),
        ("app.gather.town", .genericBrowser),
        ("around.co/", .genericBrowser),
    ]

    static func nativePlatform(forBundleID bundleID: String) -> MeetingPlatform? {
        let id = bundleID.lowercased()
        guard !id.isEmpty else { return nil }
        for app in nativeApps where app.bundlePrefixes.contains(where: id.hasPrefix) {
            return app.platform
        }
        return nil
    }

    static func browser(forBundleID bundleID: String) -> BrowserApp? {
        let id = bundleID.lowercased()
        guard !id.isEmpty else { return nil }
        return browsers.first { id.hasPrefix($0.bundlePrefix) }
    }

    /// Recognise a meeting from a URL or window title.
    static func platform(forURLOrTitle text: String) -> MeetingPlatform? {
        guard !text.isEmpty else { return nil }
        let haystack = text.lowercased()
        for entry in urlPatterns where haystack.contains(entry.pattern) {
            return entry.platform
        }
        return nil
    }
}
