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
        expectEqual(MeetingCatalog.confidence(isBrowser: true, playingAudio: true,
                                              usingMic: false, recognised: nil),
                    0, "a browser playing audio is not a meeting on its own")
        expectEqual(MeetingCatalog.confidence(isBrowser: true, playingAudio: false,
                                              usingMic: true, recognised: .meet),
                    MeetingCatalog.recordThreshold, "a recognised call with the mic records")

        print("\n\(failures == 0 ? "PASS" : "FAIL") — \(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)
    }
}
