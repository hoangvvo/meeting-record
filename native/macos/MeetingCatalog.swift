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

/// Meeting classification shared with the Windows backend in
/// `native/common/meeting-catalog.c`. This backend supplies bundle ids, titles,
/// URLs and audio activity.
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

    /// The meeting a window title names, and the page title without browser
    /// suffixes.
    static func platform(forTitle title: String) -> (platform: MeetingPlatform?, title: String) {
        var length = 0
        let matched = known(mrec_catalog_title_platform(title, &length))
        return (matched, String(decoding: title.utf8.prefix(length), as: UTF8.self))
    }

    /// How likely audio activity is a meeting; 0 when it is not one.
    static func confidence(isBrowser: Bool, playingAudio: Bool, usingMic: Bool,
                           recognised: MeetingPlatform?) -> Int32 {
        mrec_catalog_confidence(isBrowser ? 1 : 0, playingAudio ? 1 : 0, usingMic ? 1 : 0,
                                (recognised ?? .unknown).rawValue)
    }

    private static func known(_ value: Int32) -> MeetingPlatform? {
        guard let platform = MeetingPlatform(rawValue: value), platform != .unknown else {
            return nil
        }
        return platform
    }
}
