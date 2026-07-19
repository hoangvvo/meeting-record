import CoreAudio
import Foundation

/// Regression tests for tap selection semantics. These only construct tap
/// descriptions, so they need neither an audio source nor TCC permission.
@main
struct TapDescriptionTests {
    static var failures = 0

    static func expect(_ condition: Bool, _ description: String) {
        if condition {
            print("  ok   \(description)")
        } else {
            print("  FAIL \(description)")
            failures += 1
        }
    }

    static func main() {
        do {
            let mono = try TapCapture.makeTapDescription(
                pids: [], globalMixdown: true, mono: true, muteCapturedOutput: false)
            expect(mono.isExclusive,
                   "mono system tap treats its empty process list as exclusions")
            expect(mono.processes.isEmpty, "mono system tap excludes no processes")

            let stereo = try TapCapture.makeTapDescription(
                pids: [], globalMixdown: true, mono: false, muteCapturedOutput: false)
            expect(stereo.isExclusive,
                   "stereo system tap treats its empty process list as exclusions")
            expect(stereo.processes.isEmpty, "stereo system tap excludes no processes")
        } catch {
            print("  FAIL constructing system tap descriptions: \(error)")
            failures += 1
        }

        print("\n\(failures == 0 ? "PASS" : "FAIL") — \(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)
    }
}
