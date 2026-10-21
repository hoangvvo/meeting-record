import AVFoundation
import CoreAudio
import Foundation

/// CoreAudio process tap feeding a private aggregate device.
final class TapCapture {
    struct Format {
        var sampleRate: Double
        var channels: UInt32
    }

    private(set) var format = Format(sampleRate: 0, channels: 0)

    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var ioProcID: AudioDeviceIOProcID?
    private var prepared = false
    private var started = false
    private var maximumFrameCount: UInt32 = 0
    private var globalDescription: CATapDescription?
    private let configurationLock = NSLock()

    private let stateBox: CaptureStateBox
    let identifier = UUID()
    private let onConfigurationChange: (UUID, mrec_track_reason, String) -> Void
    private let monitorQueue = DispatchQueue(label: "meetingrecord.capture-monitor")
    private var monitors: [PropertyMonitor] = []

    private struct PropertyMonitor {
        var object: AudioObjectID
        var address: AudioObjectPropertyAddress
        var block: AudioObjectPropertyListenerBlock
    }

    init(stateBox: CaptureStateBox,
         onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void =
             { _, _, _ in }) {
        self.stateBox = stateBox
        self.onConfigurationChange = onConfigurationChange
    }

    deinit {
        teardown()
    }

    // MARK: - Lifecycle

    func start(pids: [pid_t],
               globalMixdown: Bool,
               mono: Bool,
               muteCapturedOutput: Bool) throws {
        try prepare(pids: pids,
                    globalMixdown: globalMixdown,
                    mono: mono,
                    muteCapturedOutput: muteCapturedOutput)
        try startPrepared()
    }

    /// Prepare HAL objects before installing host callbacks; permission checks may block.
    func prepare(pids: [pid_t],
                 globalMixdown: Bool,
                 mono: Bool,
                 muteCapturedOutput: Bool) throws {
        precondition(!prepared && !started, "already prepared")

        let description = try Self.makeTapDescription(pids: pids,
                                                      globalMixdown: globalMixdown,
                                                      mono: mono,
                                                      muteCapturedOutput: muteCapturedOutput,
                                                      excludedProcesses: globalMixdown
                                                          ? AudioProcessRegistry.recorderProcessObjects() : [])
        globalDescription = description.isExclusive ? description : nil

        var tap = AudioObjectID(kAudioObjectUnknown)
        let tapErr = AudioHardwareCreateProcessTap(description, &tap)
        guard tapErr == noErr, tap != kAudioObjectUnknown else {
            throw CaptureError.tapFailed(tapErr)
        }
        tapID = tap

        // The aggregate references the tap by UID string.
        let tapUID = HAL.string(tap, kAudioTapPropertyUID) ?? description.uuid.uuidString

        guard let output = HAL.defaultOutputDevice() else {
            teardown()
            throw CaptureError.deviceFailed(kAudioHardwareBadDeviceError)
        }

        let aggregateDescription: [String: Any] = [
            kAudioAggregateDeviceNameKey: "meeting-record-capture",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            // Clock source only; not in the sub-device list, so the aggregate
            // exposes the tap's input stream and no output path.
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
        let aggErr = AudioHardwareCreateAggregateDevice(aggregateDescription as CFDictionary,
                                                       &aggregate)
        guard aggErr == noErr, aggregate != kAudioObjectUnknown else {
            teardown()
            throw CaptureError.deviceFailed(aggErr)
        }
        aggregateID = aggregate

        // `kAudioTapPropertyFormat` can disagree with what the device delivers,
        // so read the aggregate's first input stream instead.
        guard let resolved = firstInputStreamFormat(of: aggregate) else {
            teardown()
            throw CaptureError.deviceFailed(kAudioDeviceUnsupportedFormatError)
        }
        guard resolved.mFormatID == kAudioFormatLinearPCM,
              resolved.mBitsPerChannel == 32,
              resolved.mFormatFlags & kAudioFormatFlagIsFloat != 0 else {
            teardown()
            throw CaptureError.deviceFailed(kAudioDeviceUnsupportedFormatError)
        }
        format = Format(sampleRate: resolved.mSampleRate, channels: resolved.mChannelsPerFrame)
        maximumFrameCount = max(
            HAL.value(aggregate, kAudioDevicePropertyBufferFrameSize,
                      default: UInt32(4096)),
            4096)

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
        prepared = true
    }

    func startPrepared() throws {
        precondition(prepared && !started, "capture is not prepared")
        guard aggregateID != kAudioObjectUnknown, let proc = ioProcID else {
            teardown()
            throw CaptureError.ioProcFailed(kAudioHardwareBadObjectError)
        }
        stateBox.updateFormat(sampleRate: format.sampleRate,
                              channels: format.channels,
                              maximumFrameCount: maximumFrameCount)
        let startErr = AudioDeviceStart(aggregateID, proc)
        guard startErr == noErr else {
            teardown()
            throw CaptureError.ioProcFailed(startErr)
        }
        started = true
        installMonitors()
    }

    func stop() {
        teardown()
    }

    private func teardown() {
        removeMonitors()
        configurationLock.lock()
        defer { configurationLock.unlock() }
        globalDescription = nil
        if let proc = ioProcID, aggregateID != kAudioObjectUnknown {
            if started { AudioDeviceStop(aggregateID, proc) }
            AudioDeviceDestroyIOProcID(aggregateID, proc)
        }
        ioProcID = nil
        prepared = false
        started = false
        maximumFrameCount = 0

        if aggregateID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateID)
            aggregateID = AudioObjectID(kAudioObjectUnknown)
        }
        if tapID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = AudioObjectID(kAudioObjectUnknown)
        }
    }

    private func installMonitors() {
        if globalDescription != nil {
            var address = HAL.address(kAudioHardwarePropertyProcessObjectList)
            let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
                self?.refreshSystemExclusions()
            }
            if AudioObjectAddPropertyListenerBlock(HAL.system, &address, monitorQueue, block) == noErr {
                monitors.append(PropertyMonitor(object: HAL.system, address: address, block: block))
            }
            refreshSystemExclusions()
        }
        addMonitor(object: HAL.system,
                   address: HAL.address(kAudioHardwarePropertyDefaultOutputDevice),
                   cause: MREC_TRACK_REASON_DEVICE_CHANGED,
                   reason: "default output device changed")
        addMonitor(object: aggregateID,
                   address: HAL.address(kAudioDevicePropertyDeviceIsAlive),
                   cause: MREC_TRACK_REASON_DEVICE_REMOVED,
                   reason: "capture aggregate device changed")
        addMonitor(object: aggregateID,
                   address: HAL.address(kAudioDevicePropertyNominalSampleRate),
                   cause: MREC_TRACK_REASON_FORMAT_CHANGED,
                   reason: "capture sample rate changed")
        addMonitor(object: tapID,
                   address: HAL.address(kAudioTapPropertyFormat),
                   cause: MREC_TRACK_REASON_FORMAT_CHANGED,
                   reason: "process tap format changed")
    }

    private func refreshSystemExclusions() {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        guard let description = globalDescription, tapID != kAudioObjectUnknown else { return }
        let processes = AudioProcessRegistry.recorderProcessObjects()
        guard processes != description.processes else { return }
        description.processes = processes
        var updated = Unmanaged.passUnretained(description).toOpaque()
        var address = HAL.address(kAudioTapPropertyDescription)
        let status = AudioObjectSetPropertyData(tapID, &address, 0, nil,
                                               UInt32(MemoryLayout<UnsafeMutableRawPointer>.size), &updated)
        if status != noErr {
            onConfigurationChange(identifier, MREC_TRACK_REASON_UNKNOWN,
                                  "could not update recorder audio exclusions")
        }
    }

    private func addMonitor(object: AudioObjectID,
                            address initialAddress: AudioObjectPropertyAddress,
                            cause: mrec_track_reason,
                            reason: String) {
        guard object != kAudioObjectUnknown else { return }
        var address = initialAddress
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            self.onConfigurationChange(self.identifier, cause, reason)
        }
        if AudioObjectAddPropertyListenerBlock(object, &address, monitorQueue, block) == noErr {
            monitors.append(PropertyMonitor(object: object, address: address, block: block))
        }
    }

    private func removeMonitors() {
        for monitor in monitors {
            var address = monitor.address
            AudioObjectRemovePropertyListenerBlock(monitor.object, &address,
                                                   monitorQueue, monitor.block)
        }
        monitors.removeAll()
    }

    // MARK: - Helpers

    static func makeTapDescription(pids: [pid_t],
                                   globalMixdown: Bool,
                                   mono: Bool,
                                   muteCapturedOutput: Bool,
                                   excludedProcesses: [AudioObjectID]) throws -> CATapDescription {
        let description: CATapDescription
        if globalMixdown {
            description = mono
                ? CATapDescription(monoGlobalTapButExcludeProcesses: excludedProcesses)
                : CATapDescription(stereoGlobalTapButExcludeProcesses: excludedProcesses)
        } else {
            // The tap API wants HAL process object ids, not Unix pids. Dead pids
            // resolve to objects that yield silence, so drop them first.
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
        description.muteBehavior = muteCapturedOutput ? .muted : .unmuted
        // Exclusive means an exclusion list; an empty one captures all processes.
        description.isExclusive = globalMixdown
        // Publish the tap UID so the private aggregate can resolve it.
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
    case microphonePermission
    case microphoneFailed(String)

    var status: Int32 {
        switch self {
        case .tapFailed: return -6      // MREC_ERR_TAP_FAILED
        case .deviceFailed: return -7   // MREC_ERR_DEVICE_FAILED
        case .ioProcFailed: return -8   // MREC_ERR_IOPROC_FAILED
        case .noProcesses: return -5    // MREC_ERR_NO_PROCESSES
        case .microphonePermission: return -2
        case .microphoneFailed: return -7
        }
    }

    var message: String {
        switch self {
        case .tapFailed(let e): return "AudioHardwareCreateProcessTap failed: \(fourCC(e))"
        case .deviceFailed(let e): return "aggregate device setup failed: \(fourCC(e))"
        case .ioProcFailed(let e): return "IOProc setup failed: \(fourCC(e))"
        case .noProcesses: return "none of the requested pids are doing audio IO"
        case .microphonePermission: return "microphone permission is not granted"
        case .microphoneFailed(let message): return "microphone setup failed: \(message)"
        }
    }

    /// CoreAudio errors are packed four-character codes.
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

/// Runs on a CoreAudio realtime thread: no allocation, no locks. Forwards the
/// buffer to the host's callback.
private let captureIOProc: AudioDeviceIOProc = {
    _, _, inInputData, inInputTime, _, _, clientData in

    guard let clientData else { return noErr }
    let state = Unmanaged<CaptureStateBox>.fromOpaque(clientData).takeUnretainedValue()

    let buffers = UnsafeMutableAudioBufferListPointer(
        UnsafeMutablePointer(mutating: inInputData))
    let hostTime = inInputTime.pointee.mFlags.contains(.hostTimeValid)
        ? inInputTime.pointee.mHostTime : nil
    state.emit(buffers, hostTime: hostTime)
    return noErr
}
