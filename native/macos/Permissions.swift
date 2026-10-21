import AVFoundation
import CoreAudio
import Foundation

/// Probe audio-capture permission with a temporary tap; no preflight API exists.
enum Permissions {
    /// Cache terminal states and allow only one probe; unanswered requests can block indefinitely.
    private static let cacheLock = NSLock()
    private static var cachedStatus: Int32 = 0
    private static var probeInFlight = false

    /// Returns GRANTED or DENIED when knowable within `timeout`, UNKNOWN when the
    /// probe blocks — meaning TCC has not been asked and a GUI prompt is required.
    static func audioCaptureStatus(timeout: TimeInterval = 0.05) -> Int32 {
        cacheLock.lock()
        let cached = cachedStatus
        cacheLock.unlock()
        if cached != 0 { return cached }

        let completion = beginProbeIfNeeded()
        if let completion {
            _ = completion.wait(timeout: .now() + timeout)
        }
        cacheLock.lock()
        let status = cachedStatus
        cacheLock.unlock()
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

    /// Request off the main thread. Requires NSAudioCaptureUsageDescription; poll for the result.
    static func requestAudioCapture() -> Int32 {
        guard #available(macOS 14.2, *) else { return -1 /* UNSUPPORTED_OS */ }
        _ = beginProbeIfNeeded()
        return 0
    }

    static func noteAudioCaptureGranted() {
        cacheLock.lock()
        cachedStatus = 1
        cacheLock.unlock()
    }

    /// Starts the one allowed blocking probe and returns a completion semaphore
    /// to the caller that won the race. Other callers observe UNKNOWN immediately.
    private static func beginProbeIfNeeded() -> DispatchSemaphore? {
        cacheLock.lock()
        if cachedStatus != 0 || probeInFlight {
            cacheLock.unlock()
            return nil
        }
        probeInFlight = true
        cacheLock.unlock()

        let completion = DispatchSemaphore(value: 0)
        Thread.detachNewThread {
            let status = probeBlocking()
            cacheLock.lock()
            if status == 1 || (status != 0 && cachedStatus == 0) {
                cachedStatus = status
            }
            probeInFlight = false
            cacheLock.unlock()
            completion.signal()
        }
        return completion
    }

    static func microphoneStatus() -> Int32 {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            return 1
        case .denied, .restricted:
            return 2
        case .notDetermined:
            return 0
        @unknown default:
            return 0
        }
    }

    static func requestMicrophone() -> Int32 {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            return 0
        case .denied, .restricted:
            return 0
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { _ in }
            return 0
        @unknown default:
            return -9
        }
    }
}

/// Only whether IO starts matters, not the samples.
private let probeIOProc: AudioDeviceIOProc = { _, _, _, _, _, _, _ in noErr }
