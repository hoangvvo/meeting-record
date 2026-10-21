import CoreAudio
import Foundation

/// Capture-core regressions that need neither an audio source nor TCC permission.
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
                pids: [], globalMixdown: true, mono: true, muteCapturedOutput: false,
                excludedProcesses: [11, 22])
            expect(mono.isExclusive,
                   "mono system tap uses an exclusion list")
            expect(mono.processes == [11, 22], "mono system tap excludes recorder audio objects")

            let stereo = try TapCapture.makeTapDescription(
                pids: [], globalMixdown: true, mono: false, muteCapturedOutput: false,
                excludedProcesses: [11, 22])
            expect(stereo.isExclusive,
                   "stereo system tap uses an exclusion list")
            expect(stereo.processes == [11, 22], "stereo system tap excludes recorder audio objects")
        } catch {
            print("  FAIL constructing system tap descriptions: \(error)")
            failures += 1
        }

        do {
            _ = try TapCapture.makeTapDescription(
                pids: [], globalMixdown: false, mono: true, muteCapturedOutput: false,
                excludedProcesses: [])
            expect(false, "an empty process target never becomes a global tap")
        } catch CaptureError.noProcesses {
            expect(true, "an empty process target never becomes a global tap")
        } catch {
            expect(false, "an empty process target reports no processes")
        }

        print("\n\(failures == 0 ? "PASS" : "FAIL") — \(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)
    }
}
