import Foundation

/// Deterministic tests for the classification logic — the part of detection that
/// does not need a live meeting to verify.
///
/// Compiled together with the macOS sources (see scripts/build-macos.sh), so it
/// tests the real catalog rather than a copy.

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
        // A browser must not be classified as a native conferencing client, or every
        // YouTube tab becomes a meeting.
        expectEqual(MeetingCatalog.nativePlatform(forBundleID: "com.google.Chrome"), nil,
                    "browsers are not native meeting apps")

        print("\nbrowser matching")
        expectEqual(MeetingCatalog.browser(forBundleID: "com.google.Chrome")?.bundlePrefix,
                    "com.google.chrome", "Chrome")
        expectEqual(MeetingCatalog.browser(forBundleID: "com.google.Chrome.helper")?.bundlePrefix,
                    "com.google.chrome", "Chrome helper")
        expectEqual(MeetingCatalog.browser(forBundleID: "com.apple.Safari")?.bundlePrefix,
                    "com.apple.safari", "Safari")
        expectEqual(MeetingCatalog.browser(forBundleID: "company.thebrowser.Browser")?.bundlePrefix,
                    "company.thebrowser.browser", "Arc")
        expect(MeetingCatalog.browser(forBundleID: "us.zoom.xos") == nil,
               "Zoom is not a browser")

        // Stable ids are part of the contract: consumers branch on them.
        print("\nplatform identifiers are stable")
        expectEqual(MeetingPlatform.zoom.id, "zoom", "zoom")
        expectEqual(MeetingPlatform.teams.id, "teams", "teams")
        expectEqual(MeetingPlatform.meet.id, "meet", "meet")
        expectEqual(MeetingPlatform.webex.id, "webex", "webex")
        expectEqual(MeetingPlatform.slack.id, "slack", "slack")
        expectEqual(MeetingPlatform.discord.id, "discord", "discord")
        expectEqual(MeetingPlatform.genericBrowser.id, "browser", "browser")
        expectEqual(MeetingPlatform.unknown.id, "unknown", "unknown")

        print("\nmeeting URL recognition")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "https://meet.google.com/abc-defg-hij"),
                    .meet, "Google Meet URL")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "https://acme.zoom.us/j/91234567890"),
                    .zoom, "Zoom web client URL")
        expectEqual(MeetingCatalog.platform(
            forURLOrTitle: "https://teams.microsoft.com/l/meetup-join/19%3ameeting"),
                    .teams, "Teams web URL")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "https://app.slack.com/huddle/T01/C02"),
                    .slack, "Slack huddle URL")
        // Titles are often all the Accessibility tree exposes, so they go through the
        // same matcher.
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "Meet - abc-defg-hij"), nil,
                    "a bare Meet title without the host does not match")
        expectEqual(MeetingCatalog.platform(
            forURLOrTitle: "Weekly sync | meet.google.com/xyz - Google Chrome"),
                    .meet, "host inside a window title matches")

        print("\nnon-meeting URLs must not match")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "https://www.youtube.com/watch?v=x"),
                    nil, "YouTube")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "https://zoom.us/pricing"), nil,
                    "Zoom marketing page is not a meeting")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: "https://news.ycombinator.com"), nil,
                    "unrelated site")
        expectEqual(MeetingCatalog.platform(forURLOrTitle: ""), nil, "empty string")

        print("\nconfidence threshold")
        // A known app merely being open must not be enough to start recording.
        expect(MeetingDetector.recordThreshold > 40,
               "app-present alone (40) is below the record threshold")
        // App + audio output + mic is a live two-way call.
        expect(40 + 30 + 25 >= MeetingDetector.recordThreshold,
               "app + output + mic reaches the record threshold")
        // App + output but no mic (someone talking, user muted) should still record.
        expect(40 + 30 >= MeetingDetector.recordThreshold,
               "app + output alone reaches the record threshold")

        print("\n\(failures == 0 ? "PASS" : "FAIL") — \(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)

    }
}
