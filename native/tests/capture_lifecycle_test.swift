import Foundation

private let eventCallback: mrec_track_event_callback = { event, _ in
    if let event { TestSources.read { TestSources.events.append(event.pointee) } }
}
private func waitUntil(_ condition: () -> Bool) {
    let deadline = Date().addingTimeInterval(5)
    while !condition() && Date() < deadline { Thread.sleep(forTimeInterval: 0.01) }
    precondition(condition(), "timed out waiting for capture recovery")
}
@main struct CaptureLifecycleTests {
    static func main() {
        precondition(mrec_start_tracks_raw(nil, 0, 1, 1, 0, 1, nil, nil, nil, nil, eventCallback, nil) == 0)
        let microphone = TestSources.read { TestSources.microphone! }
        TestSources.read { TestSources.microphoneAvailable = false }
        microphone.change(microphone.identifier, MREC_TRACK_REASON_DEVICE_REMOVED, "unplugged")
        waitUntil { TestSources.read { TestSources.events.contains { $0.type == MREC_TRACK_INTERRUPTED } } }
        var rate = 0.0
        var channels: UInt32 = 0
        precondition(mrec_is_running() == 1)
        precondition(mrec_current_microphone_format(&rate, &channels) == 0)
        precondition(rate == 48000 && channels == 1)
        TestSources.read { TestSources.microphoneAvailable = true }
        waitUntil { TestSources.read { TestSources.events.contains { $0.type == MREC_TRACK_RESUMED } } }
        precondition(mrec_stop() == 0)

        TestSources.read { TestSources.prepared = []; TestSources.events = [] }
        // Detected meetings provide the owner plus the current helper PIDs.
        let pids: [UInt32] = [123, 456]
        let status = pids.withUnsafeBufferPointer {
            mrec_start_tracks_raw($0.baseAddress, $0.count, 0, 1, 0, 0,
                                  nil, nil, nil, nil, eventCallback, nil)
        }
        precondition(status == 0)
        precondition(TestSources.read { TestSources.prepared.last == [123, 456] })
        TestSources.read { TestSources.alive = [123, 789]; TestSources.helpers = [789] }
        waitUntil { TestSources.read { TestSources.prepared.last == [123, 789] } }
        TestSources.read { TestSources.alive = [] }
        waitUntil { TestSources.read { TestSources.events.contains { $0.reason == MREC_TRACK_REASON_APP_EXITED } } }
        precondition(mrec_is_running() == 1)
        let count = TestSources.read { TestSources.prepared.count }
        TestSources.read { TestSources.alive = [123, 999]; TestSources.helpers = [999] }
        Thread.sleep(forTimeInterval: 1.1)
        precondition(TestSources.read { TestSources.prepared.count == count })
        precondition(mrec_stop() == 0)
        print("capture lifecycle tests passed")
    }
}
