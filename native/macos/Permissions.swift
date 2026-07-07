import AVFoundation
import CoreAudio
import Foundation

/// Audio-capture permission handling for CoreAudio process taps.
///
/// Taps are gated by `kTCCServiceAudioCapture` — a separate TCC service from
/// microphone and from screen recording, surfaced in System Settings under
/// "Screen & System Audio Recording".
///
/// Apple ships no preflight/request API for it, which leaves two awkward facts:
///
///  * **Status can only be probed.** We build a throwaway tap + aggregate and
///    see whether IO actually starts.
///
///  * **The prompt needs a GUI process.** In a process with no `NSApplication`
///    running, TCC has nowhere to draw the dialog and the CoreAudio call blocks
///    indefinitely instead of failing. Calling from the main thread deadlocks for
///    the same reason: the prompt needs the main run loop to be serviced.
enum Permissions {
    /// A grant cannot be revoked without restarting the app, so once we have seen
    /// GRANTED we never probe again. Probing is not free: it builds and tears down
    /// a real tap, and doing that next to a live capture can disturb it.
    private static let cacheLock = NSLock()
    private static var cachedGranted = false

    /// Probe the grant without ever blocking the caller.
    ///
    /// Returns GRANTED / DENIED when the answer is knowable within `timeout`, and
    /// UNKNOWN when the probe blocks — which is itself the signal that TCC has not
    /// been asked yet and a GUI prompt is required.
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

    /// The probe itself. Blocks indefinitely when the grant is undetermined and
    /// the process cannot present a prompt, so only reach it via `withTimeout`.
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

        // A denied tap still yields a device with an input stream, so the stream
        // count proves nothing. Whether IO *starts* is the real signal.
        var proc: AudioDeviceIOProcID?
        guard AudioDeviceCreateIOProcID(aggregate, probeIOProc, nil, &proc) == noErr,
              let proc else { return 2 }
        defer { AudioDeviceDestroyIOProcID(aggregate, proc) }

        guard AudioDeviceStart(aggregate, proc) == noErr else { return 2 }
        AudioDeviceStop(aggregate, proc)
        return 1 // GRANTED
    }

    /// Trigger the one-time prompt.
    ///
    /// The prompt is raised by the same CoreAudio path we use to capture, so this
    /// just performs a probe off the main thread. Requires
    /// `NSAudioCaptureUsageDescription` in the host bundle's Info.plist; without
    /// it the system refuses to prompt at all.
    ///
    /// Returns immediately — poll `audioCaptureStatus()` for the user's answer.
    static func requestAudioCapture() -> Int32 {
        guard #available(macOS 14.2, *) else { return -1 /* UNSUPPORTED_OS */ }
        // Deliberately the blocking probe on a detached thread: raising the
        // prompt is the whole point, and it must not run on the main thread or
        // the run loop that has to draw the dialog is the one we are blocking.
        Thread.detachNewThread {
            _ = probeBlocking()
        }
        return 0
    }
}

/// No-op: the probe cares only whether IO starts, not about the samples.
private let probeIOProc: AudioDeviceIOProc = { _, _, _, _, _, _, _ in noErr }
