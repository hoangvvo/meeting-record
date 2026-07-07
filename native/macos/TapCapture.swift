import AVFoundation
import CoreAudio
import Foundation

/// System-audio capture via CoreAudio process taps (macOS 14.2+).
///
/// Pipeline:
///   1. `AudioHardwareCreateProcessTap` with a `CATapDescription` — either a
///      mixdown of specific processes or a global mixdown.
///   2. A **private** aggregate device that lists the tap in its tap list. The
///      default output device is set as the aggregate's *main* sub-device purely
///      to supply a clock; the sub-device list stays empty so we never take over
///      the user's output path.
///   3. `AudioDeviceCreateIOProcID` on the aggregate to pull interleaved float32.
///
/// Two behaviours here are load-bearing and cost real time to discover:
///
///   * **Never use `AudioDeviceCreateIOProcIDWithBlock`.** On a tap-backed
///     aggregate the block variant returns `noErr`, the device reports
///     `IsRunning == 0`, and the block is never invoked. Before the TCC grant
///     exists it is worse: the call blocks forever inside
///     `HALC_ProxyIOContext::_TellServerAboutStreamUsage`. The C function
///     pointer form works in both cases.
///
///   * **Prefer a process-list tap over a global tap.** A global tap sits after
///     the hardware mix, so it records digital silence whenever the user mutes
///     their speakers — fatal for a meeting recorder. A process tap reads each
///     app's stream before that point and keeps working while muted.
final class TapCapture {
    struct Format {
        var sampleRate: Double
        var channels: UInt32
    }

    private(set) var format = Format(sampleRate: 0, channels: 0)

    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var ioProcID: AudioDeviceIOProcID?
    private var started = false

    private let stateBox: CaptureStateBox

    init(stateBox: CaptureStateBox) {
        self.stateBox = stateBox
    }

    // MARK: - Lifecycle

    func start(pids: [pid_t],
               globalMixdown: Bool,
               mono: Bool,
               muteCapturedOutput: Bool) throws {
        precondition(!started, "already started")

        let description = try makeTapDescription(pids: pids,
                                                 globalMixdown: globalMixdown,
                                                 mono: mono,
                                                 muteCapturedOutput: muteCapturedOutput)

        var tap = AudioObjectID(kAudioObjectUnknown)
        let tapErr = AudioHardwareCreateProcessTap(description, &tap)
        guard tapErr == noErr, tap != kAudioObjectUnknown else {
            throw CaptureError.tapFailed(tapErr)
        }
        tapID = tap

        // The aggregate references the tap by UID string. Read it back from the
        // created object rather than trusting the description's UUID.
        let tapUID = HAL.string(tap, kAudioTapPropertyUID) ?? description.uuid.uuidString

        guard let output = HAL.defaultOutputDevice() else {
            teardown()
            throw CaptureError.deviceFailed(kAudioHardwareBadDeviceError)
        }

        let aggregateDescription: [String: Any] = [
            kAudioAggregateDeviceNameKey: "meeting-record-capture",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            // Clock anchor only — deliberately *not* in the sub-device list, so the
            // aggregate exposes just the tap's input stream and no output path.
            kAudioAggregateDeviceMainSubDeviceKey: output.uid,
            kAudioAggregateDeviceSubDeviceListKey: [[String: Any]](),
            // Private: invisible to other apps and to Audio MIDI Setup.
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [[
                kAudioSubTapUIDKey: tapUID,
                kAudioSubTapDriftCompensationKey: true,
            ]],
        ]

        var aggregate = AudioObjectID(kAudioObjectUnknown)
        let aggErr = AudioHardwareCreateAggregateDevice(aggregateDescription as CFDictionary,
                                                       &aggregate)
        guard aggErr == noErr, aggregate != kAudioObjectUnknown else {
            teardown()
            throw CaptureError.deviceFailed(aggErr)
        }
        aggregateID = aggregate

        // Take the format from the aggregate's first input stream. The tap's own
        // kAudioTapPropertyFormat can disagree with what the device delivers
        // (e.g. reports mono while the stream is stereo).
        guard let resolved = firstInputStreamFormat(of: aggregate) else {
            teardown()
            throw CaptureError.deviceFailed(kAudioDeviceUnsupportedFormatError)
        }
        format = Format(sampleRate: resolved.mSampleRate, channels: resolved.mChannelsPerFrame)
        stateBox.updateFormat(sampleRate: resolved.mSampleRate,
                              channels: resolved.mChannelsPerFrame)

        // C function pointer, not the block variant. See the type doc comment.
        var proc: AudioDeviceIOProcID?
        let procErr = AudioDeviceCreateIOProcID(aggregate,
                                                captureIOProc,
                                                Unmanaged.passUnretained(stateBox).toOpaque(),
                                                &proc)
        guard procErr == noErr, let proc else {
            teardown()
            throw CaptureError.ioProcFailed(procErr)
        }
        ioProcID = proc

        let startErr = AudioDeviceStart(aggregate, proc)
        guard startErr == noErr else {
            teardown()
            throw CaptureError.ioProcFailed(startErr)
        }
        started = true
    }

    func stop() {
        teardown()
    }

    private func teardown() {
        if let proc = ioProcID, aggregateID != kAudioObjectUnknown {
            if started { AudioDeviceStop(aggregateID, proc) }
            AudioDeviceDestroyIOProcID(aggregateID, proc)
        }
        ioProcID = nil
        started = false

        if aggregateID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateID)
            aggregateID = AudioObjectID(kAudioObjectUnknown)
        }
        if tapID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = AudioObjectID(kAudioObjectUnknown)
        }
    }

    // MARK: - Helpers

    private func makeTapDescription(pids: [pid_t],
                                   globalMixdown: Bool,
                                   mono: Bool,
                                   muteCapturedOutput: Bool) throws -> CATapDescription {
        let description: CATapDescription

        if globalMixdown || pids.isEmpty {
            // Empty exclusion list == tap everything the system plays.
            description = mono
                ? CATapDescription(monoGlobalTapButExcludeProcesses: [])
                : CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        } else {
            // pids are Unix pids; the tap API wants HAL process object ids. Dead
            // pids are dropped first: the HAL happily hands back objects for exited
            // processes, and a tap built over those yields silence rather than an
            // error.
            let objects = pids
                .filter(AudioProcessRegistry.isAlive)
                .compactMap { HAL.processObject(forPID: $0) }
            guard !objects.isEmpty else { throw CaptureError.noProcesses }
            description = mono
                ? CATapDescription(monoMixdownOfProcesses: objects)
                : CATapDescription(stereoMixdownOfProcesses: objects)
        }

        description.uuid = UUID()
        description.name = "meeting-record-tap"
        // .unmuted = listen in without altering what the user hears.
        description.muteBehavior = muteCapturedOutput ? .muted : .unmuted
        // Non-exclusive so other processes can tap the same audio concurrently.
        description.isExclusive = false
        // NOT private. The aggregate device resolves the tap by UID string, and a
        // private tap is not published for that lookup: the aggregate is created
        // happily, exposes an input stream, and delivers nothing but zeros. The
        // *aggregate* is still private, which is what actually keeps us out of
        // other apps' device lists.
        description.isPrivate = false
        return description
    }

    private func firstInputStreamFormat(of device: AudioObjectID) -> AudioStreamBasicDescription? {
        let streams = HAL.array(device, kAudioDevicePropertyStreams,
                                scope: kAudioObjectPropertyScopeInput,
                                of: AudioObjectID.self)
        guard let stream = streams.first else { return nil }
        var addr = HAL.address(kAudioStreamPropertyVirtualFormat)
        var asbd = AudioStreamBasicDescription()
        var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        guard AudioObjectGetPropertyData(stream, &addr, 0, nil, &size, &asbd) == noErr,
              asbd.mSampleRate > 0, asbd.mChannelsPerFrame > 0 else { return nil }
        return asbd
    }
}

enum CaptureError: Error {
    case tapFailed(OSStatus)
    case deviceFailed(OSStatus)
    case ioProcFailed(OSStatus)
    case noProcesses

    var status: Int32 {
        switch self {
        case .tapFailed: return -6      // MREC_ERR_TAP_FAILED
        case .deviceFailed: return -7   // MREC_ERR_DEVICE_FAILED
        case .ioProcFailed: return -8   // MREC_ERR_IOPROC_FAILED
        case .noProcesses: return -5    // MREC_ERR_NO_PROCESSES
        }
    }

    var message: String {
        switch self {
        case .tapFailed(let e): return "AudioHardwareCreateProcessTap failed: \(fourCC(e))"
        case .deviceFailed(let e): return "aggregate device setup failed: \(fourCC(e))"
        case .ioProcFailed(let e): return "IOProc setup failed: \(fourCC(e))"
        case .noProcesses: return "none of the requested pids are doing audio IO"
        }
    }

    /// CoreAudio errors are packed four-character codes; render both forms.
    private func fourCC(_ status: OSStatus) -> String {
        let v = UInt32(bitPattern: status)
        let bytes = [UInt8(truncatingIfNeeded: v >> 24), UInt8(truncatingIfNeeded: v >> 16),
                     UInt8(truncatingIfNeeded: v >> 8), UInt8(truncatingIfNeeded: v)]
        if bytes.allSatisfy({ $0 >= 0x20 && $0 < 0x7F }) {
            return "'\(String(decoding: bytes, as: UTF8.self))' (\(status))"
        }
        return "\(status)"
    }
}

/// Realtime IOProc. Runs on a CoreAudio thread: no allocation, no locks, no
/// Swift runtime calls that could allocate. It only forwards the buffer to the
/// C callback registered by the host.
private let captureIOProc: AudioDeviceIOProc = {
    _, _, inInputData, _, _, _, clientData in

    guard let clientData else { return noErr }
    let state = Unmanaged<CaptureStateBox>.fromOpaque(clientData).takeUnretainedValue()

    let buffers = UnsafeMutableAudioBufferListPointer(
        UnsafeMutablePointer(mutating: inInputData))
    for buffer in buffers {
        guard let data = buffer.mData, buffer.mDataByteSize > 0 else { continue }
        let channels = max(buffer.mNumberChannels, 1)
        let frameCount = buffer.mDataByteSize / 4 / channels
        state.emit(data.assumingMemoryBound(to: Float.self),
                   frameCount: frameCount,
                   channels: channels)
    }
    return noErr
}
