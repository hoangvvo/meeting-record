import AVFoundation
import CoreAudio
import Foundation

/// Audio-capture permission handling for CoreAudio process taps.
///
/// Taps are gated by `kTCCServiceAudioCapture` — a separate TCC service from
/// microphone and from screen recording, surfaced in System Settings under
/// "Screen & System Audio Recording".
///
/// There is no preflight or request API, so status is probed by building a
/// throwaway tap and aggregate and checking whether IO starts.
///
/// The prompt requires a running `NSApplication`. Without one TCC has nowhere to
/// draw it and the CoreAudio call blocks instead of failing; calling from the main
/// thread deadlocks, since the prompt needs that run loop.
enum Permissions {
    /// A grant cannot be revoked without an app restart, so GRANTED is cached.
    /// Probing builds and tears down a real tap, which can disturb a live capture.
    private static let cacheLock = NSLock()
    private static var cachedGranted = false

    /// Returns GRANTED or DENIED when knowable within `timeout`, UNKNOWN when the
    /// probe blocks — meaning TCC has not been asked and a GUI prompt is required.
    static func audioCaptureStatus(timeout: TimeInterval = 4.0) -> Int32 {
        cacheLock.lock()
        let granted = cachedGranted
        cacheLock.unlock()
        if granted { return 1 }

        let status = withTimeout(seconds: timeout) { probeBlocking() } ?? 0 /* UNKNOWN */
        if status == 1 {
            cacheLock.lock()
            cachedGranted = true
            cacheLock.unlock()
        }
        return status
    }

    /// Blocks indefinitely when the grant is undetermined and no prompt can be
    /// presented. Reach it only via `withTimeout`.
    private static func probeBlocking() -> Int32 {
        guard #available(macOS 14.2, *) else { return 0 /* UNKNOWN */ }

        let description = CATapDescription(monoGlobalTapButExcludeProcesses: [])
        description.uuid = UUID()
        description.muteBehavior = .unmuted
        description.isPrivate = true

        var tap = AudioObjectID(kAudioObjectUnknown)
        guard AudioHardwareCreateProcessTap(description, &tap) == noErr,
              tap != kAudioObjectUnknown else {
            return 2 // DENIED — tap creation itself is refused
        }
        defer { AudioHardwareDestroyProcessTap(tap) }

        guard let output = HAL.defaultOutputDevice(),
              let tapUID = HAL.string(tap, kAudioTapPropertyUID) else { return 0 }

        let aggregateDescription: [String: Any] = [
            kAudioAggregateDeviceNameKey: "meeting-record-probe",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            kAudioAggregateDeviceMainSubDeviceKey: output.uid,
            kAudioAggregateDeviceSubDeviceListKey: [[String: Any]](),
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [[
                kAudioSubTapUIDKey: tapUID,
                kAudioSubTapDriftCompensationKey: true,
            ]],
        ]
        var aggregate = AudioObjectID(kAudioObjectUnknown)
        guard AudioHardwareCreateAggregateDevice(aggregateDescription as CFDictionary,
                                                &aggregate) == noErr,
              aggregate != kAudioObjectUnknown else { return 0 }
        defer { AudioHardwareDestroyAggregateDevice(aggregate) }

        // A denied tap still yields an input stream, so only whether IO starts
        // distinguishes the two.
        var proc: AudioDeviceIOProcID?
        guard AudioDeviceCreateIOProcID(aggregate, probeIOProc, nil, &proc) == noErr,
              let proc else { return 2 }
        defer { AudioDeviceDestroyIOProcID(aggregate, proc) }

        guard AudioDeviceStart(aggregate, proc) == noErr else { return 2 }
        AudioDeviceStop(aggregate, proc)
        return 1 // GRANTED
    }

    /// Trigger the one-time prompt by probing off the main thread.
    ///
    /// Requires `NSAudioCaptureUsageDescription` in the host bundle's Info.plist;
    /// without it the system does not prompt. Returns immediately — poll
    /// `audioCaptureStatus()` for the answer.
    static func requestAudioCapture() -> Int32 {
        guard #available(macOS 14.2, *) else { return -1 /* UNSUPPORTED_OS */ }
        // The blocking probe raises the prompt, and must not run on the main
        // thread, which has to draw it.
        Thread.detachNewThread {
            _ = probeBlocking()
        }
        return 0
    }
}

/// Only whether IO starts matters, not the samples.
private let probeIOProc: AudioDeviceIOProc = { _, _, _, _, _, _, _ in noErr }
