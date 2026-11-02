import CoreAudio
import Foundation

@main struct MicrophoneDeviceTests {
    static func main() {
        var inputs: [AudioObjectID] = [2]
        var fallback: AudioObjectID = 1
        var graph: [AudioObjectID: [AudioObjectID]] = [:]
        var available: Set<AudioObjectID> = [1, 2, 3]
        var appReads = 0
        let resolver = MicrophoneDeviceResolver(
            defaultInput: { fallback },
            appInputs: { pids in
                precondition(pids == [123])
                appReads += 1
                return inputs
            }, subdevices: { graph[$0] }, hasInput: { available.contains($0) },
            name: { "Microphone \($0)" })
        func route(_ source: Int32 = 2, pids: [pid_t] = [123]) -> AudioObjectID? {
            resolver.resolve(source: source, pids: pids)?.id
        }
        precondition(route() == 2)
        precondition(resolver.resolve(source: 2, pids: [123])?.name == "Microphone 2")
        let reads = appReads
        precondition(route(1) == 1 && appReads == reads, "DEFAULT never follows an app")
        precondition(route(pids: []) == 1 && appReads == reads, "global capture uses the default")

        inputs = []
        precondition(route() == 1)
        inputs = [9]
        precondition(route() == 1, "unavailable app microphones fall back")

        graph = [10: [9, 2]] // Output-only member followed by the real mic.
        inputs = [10]
        precondition(route() == 2)
        graph[11] = [10]
        inputs = [11]
        precondition(route() == 2, "unwrap nested voice-processing aggregates")
        graph[10] = [11]
        precondition(route() == 1, "aggregate cycles must terminate")
        graph[10] = []
        precondition(route() == 1)
        graph[10] = [2, 3]
        inputs = [10]
        precondition(route() == 1, "do not guess among multiple aggregate inputs")
        inputs = [10, 2]
        precondition(route() == 1, "a direct device must not hide an ambiguous aggregate")

        inputs = [2, 2]
        precondition(route() == 2)
        inputs = [2, 3]
        precondition(route() == 1, "conflicting app microphones use the default")
        inputs = [3]
        precondition(route() == 3, "app device changes must be re-resolved")
        available.remove(3)
        precondition(route() == 1)
        fallback = 2
        precondition(route() == 2)
        fallback = 0
        precondition(route() == nil)
        print("microphone device selection tests passed")
    }
}
