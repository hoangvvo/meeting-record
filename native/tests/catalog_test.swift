import Foundation

/// Meeting classification tests using the shared catalog.

@main
struct CatalogTests {
    static var failures = 0

    static func expect(_ condition: Bool, _ description: String) {
        if condition {
            print("  ok   \(description)")
        } else {
            print("  FAIL \(description)")
            failures += 1
        }
    }

    static func expectEqual<T: Equatable>(_ actual: T?, _ expected: T?,
                                          _ description: String) {
        if actual == expected {
            print("  ok   \(description)")
        } else {
            print("  FAIL \(description) — got \(String(describing: actual)), "
                + "want \(String(describing: expected))")
            failures += 1
        }
    }

    static func main() {
        print("C/Swift platform ABI")
        let platforms: [(Int32, MeetingPlatform, String)] = [
            (1, .zoom, "us.zoom.xos"), (2, .teams, "com.microsoft.teams2"),
            (4, .webex, "com.cisco.webexmeetingsapp"), (5, .slack, "com.tinyspeck.slackmacgap"),
            (6, .discord, "com.hnc.Discord"), (8, .goTo, "com.logmein.goto"),
            (9, .dialpad, "com.electron.dialpad"), (10, .whatsApp, "net.whatsapp.WhatsApp"),
            (11, .aircall, "io.aircall.workspace"), (12, .ringCentral, "com.ringcentral.glip"),
            (13, .eightByEight, "com.electron.8x8---virtual-office"),
            (14, .voov, "com.tencent.tencentmeeting"), (15, .tuple, "app.tuple.app"),
            (16, .gather, "com.gather.GatherV2"), (17, .lark, "com.larksuite.larkApp"),
            (18, .roam, "inc.wonder.roam"), (19, .clickUp, "com.clickup.desktop-app"),
            (20, .telemost, "ru.yandex.desktop.telemost"),
            (21, .telegram, "ru.keepcoder.telegram"),
            (22, .signal, "org.whispersystems.signal-desktop"),
            (23, .viber, "com.viber.osx"),
            (24, .line, "jp.naver.line.mac"),
            (25, .weChat, "com.tencent.xinwechat"),
            (26, .zalo, "com.vng.zalo"),
            (27, .jitsi, "org.jitsi.jitsi-meet"),
            (28, .element, "im.riot.app"),
            (29, .mattermost, "mattermost.desktop"),
            (30, .nextcloudTalk, "com.nextcloud.talk.mac"),
            (31, .jami, "com.savoirfairelinux.ring.macos"),
            (32, .tencentMeeting, "com.tencent.meeting"),
            (33, .weCom, "com.tencent.weworkmac"),
        ]
        for (raw, platform, bundle) in platforms {
            expectEqual(mrec_catalog_bundle_platform(bundle), raw, "C platform value for \(bundle)")
            expectEqual(platform.rawValue, raw, "Swift ABI value for \(platform)")
            expectEqual(MeetingCatalog.nativePlatform(forBundleID: bundle), platform,
                        "C classification survives Swift conversion for \(bundle)")
        }
        expectEqual(MeetingPlatform.unknown.rawValue, 0, "unknown ABI value")
        expectEqual(MeetingPlatform.meet.rawValue, 3, "Meet ABI value")
        expectEqual(MeetingPlatform.genericBrowser.rawValue, 7, "browser ABI value")

        print("native app bundle matching")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "us.zoom.xos"), .zoom,
                    "Zoom main process")
        // Helper processes are where the audio actually lives, so prefix matching must
        // reach them.
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "us.zoom.xos.cpthost"), .zoom,
                    "Zoom helper process")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "com.microsoft.teams2"), .teams,
                    "Teams 2.x client")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "com.microsoft.teams"), .teams,
                    "Teams legacy client")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "com.tinyspeck.slackmacgap.helper"),
                    .slack, "Slack helper process")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "US.ZOOM.XOS"), .zoom,
                    "matching is case-insensitive")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "com.apple.Music"), nil,
                    "unrelated app is not a meeting app")
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: ""), nil,
                    "empty bundle id is not a match")
        // Browser audio alone must not count as a native meeting.
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "com.google.Chrome"), nil,
                    "browsers are not native meeting apps")

        print("\nbrowser matching")
        expectEqual(MeetingCatalog.browser(forBundleID: "com.google.Chrome"),
                    "com.google.chrome", "Chrome")
        expectEqual(MeetingCatalog.browser(forBundleID: "com.google.Chrome.helper"),
                    "com.google.chrome", "Chrome helper")
        expectEqual(MeetingCatalog.browser(forBundleID: "com.apple.Safari"),
                    "com.apple.safari", "Safari")
        expectEqual(MeetingCatalog.browser(forBundleID: "company.thebrowser.Browser"),
                    "company.thebrowser.browser", "Arc")
        expectEqual(MeetingCatalog.browser(forBundleID: "us.zoom.xos"), nil,
                    "Zoom is not a browser")

        print("\nmeeting URL recognition")
        expectEqual(MeetingCatalog.platform(forURL: "https://meet.goto.com/123456789"),
                    .goTo, "GoTo browser call")
        expectEqual(MeetingCatalog.platform(forURL: "https://meetings.dialpad.com/room/team"),
                    .dialpad, "Dialpad browser call")
        expectEqual(MeetingCatalog.platform(forURL: "https://meet.google.com/abc-defg-hij"),
                    .meet, "Google Meet URL")
        expectEqual(MeetingCatalog.platform(forURL: "https://acme.zoom.us/j/91234567890"),
                    .zoom, "Zoom web client URL")
        expectEqual(MeetingCatalog.platform(
            forURL: "https://teams.microsoft.com/l/meetup-join/19%3ameeting"),
                    .teams, "Teams web URL")
        expectEqual(MeetingCatalog.platform(forURL: "https://app.slack.com/huddle/T01/C02"),
                    .slack, "Slack huddle URL")

        print("\nmeeting title recognition")
        let meet = MeetingCatalog.platform(forTitle: "Meet - abc-defg-hij - Audio playing")
        expectEqual(meet.platform, .meet, "a Meet title carries the meeting code")
        expectEqual(meet.title, "Meet - abc-defg-hij", "browser suffixes are dropped")
        expectEqual(MeetingCatalog.platform(forTitle: "Meet - the team").platform, nil,
                    "a Meet title needs a meeting code")
        // Titles are often all the Accessibility tree exposes, so URL fragments in a
        // title match too.
        let titled = MeetingCatalog.platform(
            forTitle: "Weekly sync | meet.google.com/abc-defg-hij - Google Chrome")
        expectEqual(titled.platform, .meet, "host inside a window title matches")
        expectEqual(titled.title, "Weekly sync | meet.google.com/abc-defg-hij - Google Chrome",
                    "other titles stay whole")

        print("\nnon-meeting URLs must not match")
        expectEqual(MeetingCatalog.platform(forURL: "https://www.youtube.com/watch?v=x"),
                    nil, "YouTube")
        expectEqual(MeetingCatalog.platform(forURL: "https://zoom.us/pricing"), nil,
                    "Zoom marketing page is not a meeting")
        expectEqual(MeetingCatalog.platform(forURL: "https://news.ycombinator.com"), nil,
                    "unrelated site")
        expectEqual(MeetingCatalog.platform(forURL: ""), nil, "empty string")

        print("\nconfidence")
        expectEqual(MeetingCatalog.confidence(isBrowser: false, playingAudio: false,
                                              usingMic: false, recognised: nil),
                    0, "an app with no audio is not a meeting")
        expectEqual(MeetingCatalog.confidence(isBrowser: false, playingAudio: true,
                                              usingMic: false, recognised: nil),
                    0, "native chat/video playback is not a meeting")
        expectEqual(MeetingCatalog.confidence(isBrowser: true, playingAudio: true,
                                              usingMic: false, recognised: nil),
                    0, "a browser playing audio is not a meeting on its own")
        expectEqual(MeetingCatalog.confidence(isBrowser: true, playingAudio: true,
                                              usingMic: false, recognised: .meet),
                    0, "a stale meeting tab plus playback is not a meeting")
        expectEqual(MeetingCatalog.confidence(isBrowser: true, playingAudio: true,
                                              usingMic: true, recognised: nil),
                    0, "unrelated microphone use and playback are not a browser meeting")
        expectEqual(MeetingCatalog.confidence(isBrowser: true, playingAudio: false,
                                              usingMic: true, recognised: .meet),
                    MeetingCatalog.recordThreshold, "a recognised call with the mic records")

        print("\n\(failures == 0 ? "PASS" : "FAIL") — \(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)
    }
}
