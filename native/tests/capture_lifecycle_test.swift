import Foundation

@_silgen_name("mrec_current_microphone_name")
private func currentMicrophoneName(_ buffer: UnsafeMutablePointer<CChar>?, _ capacity: Int) -> Int32

private let eventCallback: mrec_track_event_callback = { event, _ in
    if let event { TestSources.read { TestSources.events.append(event.pointee) } }
}
private let audioCallback: MrecAudioCallback = { _, frames, channels, rate, time, data in
    let packet = TestAudioPacket(frames: frames, channels: channels, sampleRate: rate, time: time)
    TestSources.read {
        if data == UnsafeMutableRawPointer(bitPattern: 1) {
            TestSources.systemPackets.append(packet)
        } else {
            TestSources.microphonePackets.append(packet)
        }
    }
}

private func waitUntil(_ message: String, _ condition: () -> Bool) {
    let deadline = Date().addingTimeInterval(5)
    while !condition() && Date() < deadline { Thread.sleep(forTimeInterval: 0.01) }
    precondition(condition(), message)
}

private func assertStable<T: Equatable>(_ value: () -> T) {
    let initial = value()
    let deadline = Date().addingTimeInterval(0.25)
    while Date() < deadline {
        Thread.sleep(forTimeInterval: 0.01)
        precondition(value() == initial, "stale work changed the session")
    }
}

private func start(pids: [UInt32] = [], microphone: Int32 = 1, mono: Int32 = 1) -> Int32 {
    pids.withUnsafeBufferPointer {
        mrec_start_tracks_raw(
            $0.baseAddress, $0.count, pids.isEmpty ? 1 : 0, mono, 0, microphone,
            audioCallback, UnsafeMutableRawPointer(bitPattern: 1),
            audioCallback, UnsafeMutableRawPointer(bitPattern: 2), eventCallback, nil)
    }
}

private func format(microphone: Bool = false) -> (Double, UInt32) {
    var rate = 0.0
    var channels: UInt32 = 0
    let status =
        microphone
        ? mrec_current_microphone_format(&rate, &channels)
        : mrec_current_format(&rate, &channels)
    precondition(status == 0)
    return (rate, channels)
}

private func events(_ track: mrec_track) -> [mrec_track_event] {
    TestSources.read { TestSources.events.filter { $0.track == track } }
}

private func name() -> String {
    var buffer = [CChar](repeating: 0, count: 256)
    precondition(currentMicrophoneName(&buffer, buffer.count) == 0)
    return String(cString: buffer)
}

private func run(_ body: () -> Void) {
    precondition(mrec_is_running() == 0)
    TestSources.reset()
    body()
    if mrec_is_running() != 0 { precondition(mrec_stop() == 0) }
    precondition(
        TestSources.read { TestSources.taps.values.allSatisfy { !$0.prepared && !$0.started } })
    precondition(TestSources.read { TestSources.microphones.values.allSatisfy { !$0.started } })
    precondition(mrec_current_format(nil, nil) == -4)
    precondition(mrec_current_microphone_format(nil, nil) == -4)
}

@main struct CaptureLifecycleTests {
    static func main() {
        startupFailures()
        independentTracks()
        systemRecovery()
        microphoneRecovery()
        independentRecovery()
        replacementFailures()
        staleCallbacks()
        stopDuringRecovery()
        helperReplacement()
        appExitDuringRecovery()
        print("capture lifecycle tests passed")
    }

    static func startupFailures() {
        run {
            for count in [-1, 1] {
                precondition(mrec_start_tracks_raw(nil, count, 1, 1, 0, 0,
                                                   nil, nil, nil, nil, nil, nil) == -9)
            }
            precondition(start(microphone: 3) == -9)
            precondition(start(pids: [0]) == -9)
            precondition(start(pids: [UInt32.max]) == -9)
            precondition(
                TestSources.read { TestSources.taps.isEmpty && TestSources.microphones.isEmpty })
        }
        let faults: [(Int32, () -> Void)] = [
            (-2, { TestSources.audioPermission = 2 }),
            (-2, { TestSources.microphonePermission = 2 }),
            (-2, { TestSources.preparationTimesOut = true }),
            (-6, { TestSources.tapPrepareFailures = 1 }),
            (-8, { TestSources.tapStartFailures = 1 }),
            (-7, { TestSources.microphoneStartFailures = 1 }),
        ]
        for (status, fault) in faults {
            run {
                TestSources.read { fault() }
                precondition(start() == status)
                precondition(mrec_is_running() == 0)
                precondition(mrec_stop() == -4)
                precondition(
                    TestSources.read {
                        TestSources.taps.values.allSatisfy { !$0.prepared && !$0.started }
                    })
                precondition(
                    TestSources.read { TestSources.microphones.values.allSatisfy { !$0.started } })
                TestSources.read {
                    TestSources.audioPermission = 1
                    TestSources.microphonePermission = 1
                }
                precondition(start() == 0)
                precondition(start() == -3)
                precondition(mrec_stop() == 0)
                precondition(mrec_stop() == -4)
                precondition(start(microphone: 0) == 0)
                precondition(mrec_current_microphone_format(nil, nil) == -4)
            }
        }
    }

    static func independentTracks() {
        for mono: Int32 in [0, 1] {
            run {
                precondition(start(mono: mono) == 0)
                precondition(format() == (48000, mono == 0 ? 2 : 1))
                precondition(format(microphone: true) == (44100, 1))
                let sources = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
                sources.0.emit()
                sources.1.emit()
                precondition(
                    TestSources.read {
                        let system = TestSources.systemPackets.last!
                        let microphone = TestSources.microphonePackets.last!
                        return system.frames == 2 && system.channels == (mono == 0 ? 2 : 1)
                            && microphone.frames == 2 && microphone.channels == 1
                            && system.sampleRate == 48000 && microphone.sampleRate == 44100
                            && system.time == microphone.time && system.time == 42
                            && TestSources.taps.values.allSatisfy { $0.global && $0.pids.isEmpty }
                    })
                precondition(name() == "Test microphone")
                precondition(mrec_stop() == 0)
                sources.0.emit()
                sources.1.emit()
                precondition(
                    TestSources.read {
                        TestSources.systemPackets.count == 1
                            && TestSources.microphonePackets.count == 1
                    })
            }
        }
    }

    static func systemRecovery() {
        run {
            precondition(start(mono: 0) == 0)
            let sources = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            TestSources.read { TestSources.tapAvailable = false }
            sources.0.change(sources.0.identifier, MREC_TRACK_REASON_SERVICE_RESET, "service reset")
            waitUntil("system source did not interrupt") {
                events(MREC_TRACK_SYSTEM_AUDIO).contains { $0.type == MREC_TRACK_INTERRUPTED }
            }
            precondition(mrec_is_running() == 1)
            precondition(format() == (48000, 2))
            sources.1.emit()
            precondition(
                TestSources.read {
                    TestSources.microphonePackets.count == 1 && TestSources.microphoneAttempts == 1
                })
            TestSources.read {
                TestSources.tapAvailable = true
                TestSources.tapRate = 32000
            }
            waitUntil("system source did not recover") {
                events(MREC_TRACK_SYSTEM_AUDIO).last?.type == MREC_TRACK_RESUMED
            }
            precondition(format() == (32000, 2))
            TestSources.read { TestSources.tap! }.emit()
            precondition(TestSources.read { TestSources.systemPackets.last?.sampleRate == 32000 })
            precondition(events(MREC_TRACK_MICROPHONE).isEmpty)
            precondition(TestSources.read { TestSources.taps[sources.0.identifier]?.stops == 1 })
        }
    }

    static func microphoneRecovery() {
        run {
            precondition(start() == 0)
            let sources = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            TestSources.read { TestSources.microphoneAvailable = false }
            sources.1.change(sources.1.identifier, MREC_TRACK_REASON_DEVICE_REMOVED, "unplugged")
            waitUntil("microphone did not interrupt") {
                events(MREC_TRACK_MICROPHONE).contains { $0.type == MREC_TRACK_INTERRUPTED }
            }
            precondition(mrec_is_running() == 1)
            precondition(format(microphone: true) == (44100, 1))
            precondition(name() == "Test microphone")
            sources.0.emit()
            precondition(
                TestSources.read {
                    TestSources.systemPackets.count == 1 && TestSources.tapAttempts == 1
                })
            TestSources.read {
                TestSources.microphoneName = "Replacement microphone"
                TestSources.microphoneRate = 32000
                TestSources.microphoneAvailable = true
            }
            waitUntil("microphone did not recover") {
                events(MREC_TRACK_MICROPHONE).last?.type == MREC_TRACK_RESUMED
            }
            precondition(
                events(MREC_TRACK_MICROPHONE).map(\.type) == [
                    MREC_TRACK_INTERRUPTED, MREC_TRACK_DEVICE_CHANGED, MREC_TRACK_RESUMED,
                ])
            precondition(format(microphone: true) == (32000, 1))
            precondition(name() == "Replacement microphone")
            TestSources.read { TestSources.microphone! }.emit()
            precondition(
                TestSources.read { TestSources.microphonePackets.last?.sampleRate == 32000 })
            precondition(events(MREC_TRACK_SYSTEM_AUDIO).isEmpty)
        }
    }

    static func independentRecovery() {
        run {
            precondition(start() == 0)
            let sources = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            TestSources.read {
                TestSources.tapAvailable = false
                TestSources.microphoneAvailable = false
            }
            sources.0.change(sources.0.identifier, MREC_TRACK_REASON_DEVICE_REMOVED, "output lost")
            sources.1.change(sources.1.identifier, MREC_TRACK_REASON_DEVICE_REMOVED, "input lost")
            waitUntil("both sources did not interrupt") {
                TestSources.read { TestSources.events.count == 2 }
            }
            TestSources.read { TestSources.microphoneAvailable = true }
            waitUntil("microphone recovery waited for system audio") {
                events(MREC_TRACK_MICROPHONE).last?.type == MREC_TRACK_RESUMED
            }
            precondition(events(MREC_TRACK_SYSTEM_AUDIO).map(\.type) == [MREC_TRACK_INTERRUPTED])
            TestSources.read { TestSources.microphone! }.emit()
            precondition(TestSources.read { TestSources.microphonePackets.count == 1 })
            TestSources.read { TestSources.tapAvailable = true }
            waitUntil("system audio did not recover independently") {
                events(MREC_TRACK_SYSTEM_AUDIO).last?.type == MREC_TRACK_RESUMED
            }
            precondition(mrec_is_running() == 1)
        }
    }

    static func replacementFailures() {
        run {
            precondition(start() == 0)
            let sources = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            TestSources.read {
                TestSources.tapStartFailures = 2
                TestSources.microphoneStartFailures = 2
            }
            sources.0.change(
                sources.0.identifier, MREC_TRACK_REASON_FORMAT_CHANGED, "format changed")
            sources.1.change(
                sources.1.identifier, MREC_TRACK_REASON_DEVICE_CHANGED, "input changed")
            waitUntil("failed replacements did not retry") {
                events(MREC_TRACK_SYSTEM_AUDIO).last?.type == MREC_TRACK_RESUMED
                    && events(MREC_TRACK_MICROPHONE).last?.type == MREC_TRACK_RESUMED
            }
            precondition(
                TestSources.read {
                    TestSources.tapAttempts == 4 && TestSources.microphoneAttempts == 4
                })
            precondition(
                TestSources.read {
                    TestSources.taps.values.filter(\.started).count == 1
                        && TestSources.microphones.values.filter(\.started).count == 1
                })
            precondition(
                events(MREC_TRACK_SYSTEM_AUDIO).map(\.type) == [
                    MREC_TRACK_INTERRUPTED, MREC_TRACK_RESUMED,
                ])
            precondition(
                events(MREC_TRACK_MICROPHONE).map(\.type) == [
                    MREC_TRACK_INTERRUPTED, MREC_TRACK_RESUMED,
                ])
        }
    }

    static func staleCallbacks() {
        run {
            precondition(start() == 0)
            let old = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            for _ in 0..<3 {
                old.0.change(old.0.identifier, MREC_TRACK_REASON_UNKNOWN, "duplicate")
                old.1.change(old.1.identifier, MREC_TRACK_REASON_UNKNOWN, "duplicate")
            }
            waitUntil("sources were not replaced") {
                TestSources.read {
                    TestSources.tap?.identifier != old.0.identifier
                        && TestSources.microphone?.identifier != old.1.identifier
                }
            }
            let replacement = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            old.0.change(old.0.identifier, MREC_TRACK_REASON_UNKNOWN, "stale")
            old.1.change(old.1.identifier, MREC_TRACK_REASON_UNKNOWN, "stale")
            replacement.0.change(replacement.0.identifier, MREC_TRACK_REASON_UNKNOWN, "current")
            replacement.1.change(replacement.1.identifier, MREC_TRACK_REASON_UNKNOWN, "current")
            waitUntil("current callbacks were not processed") {
                TestSources.read {
                    TestSources.tap?.identifier != replacement.0.identifier
                        && TestSources.microphone?.identifier != replacement.1.identifier
                }
            }
            assertStable {
                TestSources.read { TestSources.tapAttempts + TestSources.microphoneAttempts }
            }
            precondition(
                TestSources.read {
                    TestSources.tapAttempts == 3 && TestSources.microphoneAttempts == 3
                        && TestSources.events.isEmpty
                })
        }
    }

    static func stopDuringRecovery() {
        run {
            precondition(start() == 0)
            let old = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            TestSources.read {
                TestSources.tapAvailable = false
                TestSources.microphoneAvailable = false
            }
            old.0.change(old.0.identifier, MREC_TRACK_REASON_DEVICE_REMOVED, "lost")
            old.1.change(old.1.identifier, MREC_TRACK_REASON_DEVICE_REMOVED, "lost")
            waitUntil("retries did not advance") {
                TestSources.read {
                    TestSources.tapAttempts >= 5 && TestSources.microphoneAttempts >= 5
                }
            }
            let stoppedAt = Date()
            precondition(mrec_stop() == 0)
            precondition(Date().timeIntervalSince(stoppedAt) < 0.5)
            let oldEventCount = TestSources.read { TestSources.events.count }
            TestSources.read {
                TestSources.tapAvailable = true
                TestSources.microphoneAvailable = true
            }
            precondition(start() == 0)
            let new = TestSources.read { (TestSources.tap!, TestSources.microphone!) }
            old.0.change(old.0.identifier, MREC_TRACK_REASON_UNKNOWN, "late")
            old.1.change(old.1.identifier, MREC_TRACK_REASON_UNKNOWN, "late")
            new.0.change(new.0.identifier, MREC_TRACK_REASON_UNKNOWN, "current")
            new.1.change(new.1.identifier, MREC_TRACK_REASON_UNKNOWN, "current")
            waitUntil("old retries blocked the new session") {
                TestSources.read {
                    TestSources.tap?.identifier != new.0.identifier
                        && TestSources.microphone?.identifier != new.1.identifier
                }
            }
            assertStable {
                TestSources.read { TestSources.tapAttempts + TestSources.microphoneAttempts }
            }
            precondition(TestSources.read { TestSources.events.count == oldEventCount })
            TestSources.read { TestSources.tap! }.emit()
            TestSources.read { TestSources.microphone! }.emit()
            precondition(
                TestSources.read {
                    TestSources.systemPackets.count == 1 && TestSources.microphonePackets.count == 1
                })
        }
    }

    static func helperReplacement() {
        run {
            precondition(start(pids: [123, 456]) == 0)
            precondition(TestSources.read { TestSources.prepared.last == [123, 456] })
            TestSources.read {
                TestSources.alive = [123, 789]
                TestSources.helpers = [789]
            }
            waitUntil("new helper was not followed") {
                TestSources.read { TestSources.prepared.last == [123, 789] }
            }
            let old = TestSources.read { TestSources.tap! }
            TestSources.read { TestSources.tapAvailable = false }
            old.change(old.identifier, MREC_TRACK_REASON_UNKNOWN, "lost")
            waitUntil("helper source did not interrupt") {
                events(MREC_TRACK_SYSTEM_AUDIO).last?.type == MREC_TRACK_INTERRUPTED
            }
            TestSources.read {
                TestSources.alive = [123, 999]
                TestSources.helpers = [999]
                TestSources.tapAvailable = true
            }
            waitUntil("recovery reused a dead helper") {
                TestSources.read {
                    TestSources.tap?.identifier != old.identifier
                        && TestSources.prepared.last == [123, 999]
                }
            }
        }
    }

    static func appExitDuringRecovery() {
        for exitTiming in 0..<3 {
            run {
                precondition(start(pids: [123]) == 0)
                let old = TestSources.read { TestSources.tap! }
                if exitTiming == 1 {
                    TestSources.read { TestSources.tapAvailable = false }
                    old.change(old.identifier, MREC_TRACK_REASON_SERVICE_RESET, "service reset")
                    waitUntil("system source did not interrupt before exit") {
                        events(MREC_TRACK_SYSTEM_AUDIO).last?.type == MREC_TRACK_INTERRUPTED
                    }
                    TestSources.read {
                        TestSources.alive = []
                        TestSources.helpers = []
                        TestSources.tapAvailable = true
                    }
                } else {
                    TestSources.read {
                        if exitTiming == 2 {
                            TestSources.exitDuringHelperLookup = true
                        } else {
                            TestSources.alive = []
                            TestSources.helpers = []
                        }
                    }
                    old.change(old.identifier, MREC_TRACK_REASON_SERVICE_RESET, "service reset")
                }
                waitUntil("exited target was not reported") {
                    events(MREC_TRACK_SYSTEM_AUDIO).contains {
                        $0.reason == MREC_TRACK_REASON_APP_EXITED
                    }
                }
                precondition(
                    TestSources.read { TestSources.taps.values.filter(\.started).isEmpty },
                    "an exited process target restarted capture")
                precondition(
                    TestSources.read { !TestSources.prepared.dropFirst().contains { $0.isEmpty } },
                    "process recovery fell back to global capture")
                precondition(mrec_is_running() == 1)
                TestSources.read { TestSources.microphone! }.emit()
                precondition(TestSources.read { TestSources.microphonePackets.count == 1 })
                let attempts = TestSources.read { TestSources.tapAttempts }
                TestSources.read {
                    TestSources.alive = [123, 999]
                    TestSources.helpers = [999]
                }
                assertStable { TestSources.read { TestSources.tapAttempts } }
                precondition(TestSources.read { TestSources.tapAttempts == attempts })
                precondition(
                    events(MREC_TRACK_SYSTEM_AUDIO).last?.reason == MREC_TRACK_REASON_APP_EXITED)
            }
        }
    }
}
