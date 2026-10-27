import Foundation

/// Mirrors `mrec_platform`.
enum MeetingPlatform: Int32 {
    case unknown = 0
    case zoom = 1
    case teams = 2
    case meet = 3
    case webex = 4
    case slack = 5
    case discord = 6
    case genericBrowser = 7
}

/// Internal only; the public API reports the library's confidence/recommendation.
enum MeetingCallState: Int32 {
    case unknown = 0
    case joined = 1
    case notJoined = 2
}

struct MeetingPage {
    var platform: MeetingPlatform?
    var title = ""
    var url = ""
    var callState = MeetingCallState.unknown
}

/// Swift access to the shared meeting catalog.
enum MeetingCatalog {
    /// Below this confidence, callers should not record.
    static let recordThreshold = Int32(MREC_CATALOG_RECORD_THRESHOLD)

    /// Prefix match, so helper processes with suffixed bundle ids count.
    static func nativePlatform(forBundleID bundleID: String) -> MeetingPlatform? {
        known(mrec_catalog_bundle_platform(bundleID))
    }

    /// The browser's bundle-id prefix, which its helpers share.
    static func browser(forBundleID bundleID: String) -> String? {
        mrec_catalog_bundle_browser(bundleID).map { String(cString: $0) }
    }

    static func platform(forURL url: String) -> MeetingPlatform? {
        known(mrec_catalog_url_platform(url))
    }

    static func contextPlatform(url: String, title: String) -> MeetingPlatform? {
        known(mrec_catalog_context_platform(url, title))
    }

    static func callControl(platform: MeetingPlatform, role: String,
                            identifier: String?, label: String?) -> UInt32 {
        let controlRole: Int32
        switch role {
        case "AXButton", "AXMenuButton", "AXPopUpButton": controlRole = Int32(MREC_CONTROL_BUTTON)
        case "AXMenuItem": controlRole = Int32(MREC_CONTROL_MENU_ITEM)
        case "AXStaticText", "AXLink": controlRole = Int32(MREC_CONTROL_TEXT)
        case "AXHeading", "AXStatusBar": controlRole = Int32(MREC_CONTROL_STATUS)
        default: return 0
        }
        return mrec_catalog_call_control(platform.rawValue, controlRole, identifier ?? "", label ?? "")
    }

    /// The meeting a window title names, and the page title without browser
    /// suffixes.
    static func platform(forTitle title: String) -> (platform: MeetingPlatform?, title: String) {
        var length = 0
        let matched = known(mrec_catalog_title_platform(title, &length))
        return (matched, String(decoding: title.utf8.prefix(length), as: UTF8.self))
    }

    static func browserWindow(_ window: BrowserWindowContent) -> MeetingPage {
        let state = MeetingCallState(rawValue: mrec_catalog_call_state(window.callSignals)) ?? .unknown
        if state != .unknown, let platform = window.callPlatform {
            return MeetingPage(platform: platform, title: self.platform(forTitle: window.title).title,
                               url: window.url, callState: state)
        }
        let titles = window.tabTitles.map { strdup($0) }
        defer { titles.forEach { free($0) } }
        let pointers = titles.map { $0.map { UnsafePointer($0) } }
        let match = pointers.withUnsafeBufferPointer {
            mrec_catalog_browser_match(window.url, window.title, $0.baseAddress, $0.count)
        }
        let title = match.tab_index < 0 ? window.title : window.tabTitles[Int(match.tab_index)]
        return MeetingPage(platform: known(match.platform),
                           title: String(decoding: title.utf8.prefix(match.title_length), as: UTF8.self),
                           url: match.tab_index < 0 ? window.url : "")
    }

    /// How likely audio activity is a meeting; 0 when it is not one.
    static func confidence(isBrowser: Bool, playingAudio: Bool, usingMic: Bool,
                           recognised: MeetingPlatform?,
                           callState: MeetingCallState = .unknown) -> Int32 {
        mrec_catalog_call_confidence(isBrowser ? 1 : 0, playingAudio ? 1 : 0, usingMic ? 1 : 0,
                                     (recognised ?? .unknown).rawValue, callState.rawValue)
    }

    private static func known(_ value: Int32) -> MeetingPlatform? {
        guard let platform = MeetingPlatform(rawValue: value), platform != .unknown else {
            return nil
        }
        return platform
    }
}
