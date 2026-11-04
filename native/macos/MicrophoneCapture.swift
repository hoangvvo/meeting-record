import AVFoundation
import AudioToolbox
import CoreAudio
import Foundation

private let microphoneInputCallback: AURenderCallback = { context, flags, time, _, frames, _ in
    return Unmanaged<MicrophoneCapture>.fromOpaque(context).takeUnretainedValue()
        .render(flags: flags, time: time, frameCount: frames)
}

/// An input-only HAL unit keeps microphone IO independent of the process tap's
/// aggregate device. AVAudioEngine can stall or repeatedly reconfigure that graph.
final class MicrophoneCapture {
    struct Format {
        var sampleRate: Double
        var channels: UInt32
    }
    private(set) var format = Format(sampleRate: 0, channels: 0)
    private(set) var deviceName = ""
    private(set) var liveTargetPIDs: () -> [pid_t] = { [] }
    private let stateBox: CaptureStateBox
    let identifier = UUID()
    private let onConfigurationChange: (UUID, mrec_track_reason, String) -> Void
    private var unit: AudioUnit?
    private var samples: UnsafeMutablePointer<Float>?
    private var maximumFrames: UInt32 = 0
    private var deviceTimer: DispatchSourceTimer?
    private let deviceQueue = DispatchQueue(label: "meetingrecord.microphone-device")
    private var deviceListener: AudioObjectPropertyListenerBlock?
    private var listenerDevice = AudioObjectID(kAudioObjectUnknown)
    private let addresses = [
        HAL.address(kAudioDevicePropertyDeviceIsAlive),
        HAL.address(kAudioDevicePropertyNominalSampleRate),
        HAL.address(kAudioDevicePropertyStreamConfiguration, scope: kAudioObjectPropertyScopeInput),
    ]

    init(
        stateBox: CaptureStateBox,
        onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void = { _, _, _ in }
    ) {
        self.stateBox = stateBox
        self.onConfigurationChange = onConfigurationChange
    }

    deinit { stop() }

    func start(source: Int32, pids: [pid_t]) throws {
        guard Permissions.microphoneStatus() == 1 else { throw CaptureError.microphonePermission }
        let liveTargets = Self.targetPIDs(source: source, pids: pids)
        liveTargetPIDs = liveTargets
        let resolver = MicrophoneDeviceResolver.system
        guard let selected = resolver.resolve(source: source, pids: liveTargets()) else {
            throw CaptureError.microphoneFailed("no microphone is available")
        }
        do {
            try startUnit(selected: selected)
        } catch {
            stop()
            throw error
        }
        deviceName = resolver.name(selected.id)
        let boundDevice = MicrophoneDevice(id: selected.id, name: deviceName)
        let listener: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            self.onConfigurationChange(
                self.identifier, MREC_TRACK_REASON_DEVICE_CHANGED,
                "microphone configuration changed")
        }
        for var address in addresses {
            AudioObjectAddPropertyListenerBlock(selected.id, &address, deviceQueue, listener)
        }
        listenerDevice = selected.id
        deviceListener = listener
        let timer = DispatchSource.makeTimerSource(queue: deviceQueue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            if resolver.resolve(source: source, pids: liveTargets()) != boundDevice {
                self.onConfigurationChange(
                    self.identifier, MREC_TRACK_REASON_DEVICE_CHANGED,
                    "microphone device changed")
            }
        }
        timer.resume()
        deviceTimer = timer
    }

    static func targetPIDs(source: Int32, pids: [pid_t]) -> () -> [pid_t] {
        let targets = source == 2 ? pids : []
        let identities = Dictionary(
            uniqueKeysWithValues: targets.compactMap { pid in
                ProcessTree.identity(of: pid).map { (pid, $0) }
            })
        return {
            targets.filter { pid in
                guard let original = identities[pid] else { return false }
                return ProcessTree.identity(of: pid) == original
            }
        }
    }

    private func check(_ status: OSStatus, _ operation: String) throws {
        guard status == noErr else {
            throw CaptureError.microphoneFailed("\(operation) (\(status))")
        }
    }

    private func startUnit(selected: MicrophoneDevice) throws {
        var description = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: kAudioUnitSubType_HALOutput, componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0, componentFlagsMask: 0)
        guard let component = AudioComponentFindNext(nil, &description) else {
            throw CaptureError.microphoneFailed("microphone audio unit is unavailable")
        }
        var created: AudioUnit?
        try check(AudioComponentInstanceNew(component, &created), "could not create microphone audio unit")
        guard let created else { throw CaptureError.microphoneFailed("microphone audio unit is unavailable") }
        unit = created
        var enabled: UInt32 = 1
        var disabled: UInt32 = 0
        try check(
            AudioUnitSetProperty(
                created, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                1, &enabled, UInt32(MemoryLayout<UInt32>.size)), "could not enable microphone input")
        try check(
            AudioUnitSetProperty(
                created, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                0, &disabled, UInt32(MemoryLayout<UInt32>.size)), "could not disable microphone output")
        var device = selected.id
        try check(
            AudioUnitSetProperty(
                created, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                0, &device, UInt32(MemoryLayout<AudioObjectID>.size)), "could not select microphone")
        var hardware = AudioStreamBasicDescription()
        var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        try check(
            AudioUnitGetProperty(
                created, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                1, &hardware, &size), "could not read microphone format")
        guard hardware.mSampleRate.isFinite, hardware.mSampleRate > 0, hardware.mChannelsPerFrame > 0 else {
            throw CaptureError.microphoneFailed("microphone format is unavailable")
        }
        var client = AudioStreamBasicDescription(
            mSampleRate: hardware.mSampleRate,
            mFormatID: kAudioFormatLinearPCM, mFormatFlags: kAudioFormatFlagsNativeFloatPacked,
            mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4, mChannelsPerFrame: 1,
            mBitsPerChannel: 32, mReserved: 0)
        try check(
            AudioUnitSetProperty(
                created, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                1, &client, size), "could not configure microphone PCM")
        // Preserve the previous microphone contract: mono, using the first input
        // channel, with no output path that could feed the microphone to speakers.
        var channelMap: Int32 = 0
        try check(
            AudioUnitSetProperty(
                created, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output,
                1, &channelMap, UInt32(MemoryLayout<Int32>.size)), "could not select microphone channel")
        var callback = AURenderCallbackStruct(
            inputProc: microphoneInputCallback,
            inputProcRefCon: Unmanaged.passUnretained(self).toOpaque())
        try check(
            AudioUnitSetProperty(
                created, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                0, &callback, UInt32(MemoryLayout<AURenderCallbackStruct>.size)),
            "could not configure microphone callback")
        size = UInt32(MemoryLayout<UInt32>.size)
        try check(
            AudioUnitGetProperty(
                created, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                0, &maximumFrames, &size), "could not read microphone buffer size")
        guard maximumFrames > 0, maximumFrames <= UInt32.max / 4 else {
            throw CaptureError.microphoneFailed("microphone buffer size is invalid")
        }
        samples = .allocate(capacity: Int(maximumFrames))
        format = Format(sampleRate: hardware.mSampleRate, channels: 1)
        stateBox.updateFormat(sampleRate: hardware.mSampleRate, channels: 1)
        try check(AudioUnitInitialize(created), "could not initialize microphone")
        try check(AudioOutputUnitStart(created), "could not start microphone")
    }

    fileprivate func render(
        flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
        time: UnsafePointer<AudioTimeStamp>, frameCount: UInt32
    ) -> OSStatus {
        guard let unit, let samples, frameCount > 0 else { return noErr }
        guard frameCount <= maximumFrames else { return kAudioUnitErr_TooManyFramesToProcess }
        var buffers = AudioBufferList(
            mNumberBuffers: 1,
            mBuffers: AudioBuffer(mNumberChannels: 1, mDataByteSize: frameCount * 4, mData: samples))
        let status = AudioUnitRender(unit, flags, time, 1, frameCount, &buffers)
        if status == noErr {
            stateBox.emit(
                samples, frameCount: frameCount, channels: 1,
                hostTime: time.pointee.mFlags.contains(.hostTimeValid) ? time.pointee.mHostTime : nil)
        }
        return status
    }

    func stop() {
        deviceTimer?.cancel()
        deviceTimer = nil
        if let deviceListener {
            for var address in addresses {
                AudioObjectRemovePropertyListenerBlock(listenerDevice, &address, deviceQueue, deviceListener)
            }
            self.deviceListener = nil
        }
        if let unit {
            // Stop and dispose IO before releasing the callback's unretained
            // context or its preallocated render buffer.
            AudioOutputUnitStop(unit)
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
            self.unit = nil
        }
        samples?.deallocate()
        samples = nil
        maximumFrames = 0
    }
}
