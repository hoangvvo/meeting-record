import AVFAudio
import AVFoundation
import AudioToolbox
import CoreAudio
import Foundation

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
    private var engine: AVAudioEngine?
    private var tapped = false
    private var configurationObserver: NSObjectProtocol?
    private var deviceTimer: DispatchSourceTimer?

    init(stateBox: CaptureStateBox,
         onConfigurationChange: @escaping (UUID, mrec_track_reason, String) -> Void =
             { _, _, _ in }) {
        self.stateBox = stateBox
        self.onConfigurationChange = onConfigurationChange
    }

    func start(source: Int32, pids: [pid_t]) throws {
        guard Permissions.microphoneStatus() == 1 else {
            throw CaptureError.microphonePermission
        }

        let targets = source == 2 ? pids : []
        let identities = Dictionary(uniqueKeysWithValues: targets.compactMap { pid in
            ProcessTree.identity(of: pid).map { (pid, $0) }
        })
        let resolver = MicrophoneDeviceResolver.system
        let liveTargets = {
            targets.filter { pid in
                guard let original = identities[pid] else { return false }
                return ProcessTree.identity(of: pid) == original
            }
        }
        liveTargetPIDs = liveTargets
        guard let selected = resolver.resolve(source: source, pids: liveTargets()) else {
            throw CaptureError.microphoneFailed("no microphone is available")
        }
        let engine = AVAudioEngine()
        let input = engine.inputNode
        guard let unit = input.audioUnit else {
            throw CaptureError.microphoneFailed("microphone audio unit is unavailable")
        }
        var device = selected.id
        let status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                          kAudioUnitScope_Global, 0, &device,
                                          UInt32(MemoryLayout<AudioObjectID>.size))
        guard status == noErr else {
            throw CaptureError.microphoneFailed("could not select microphone (\(status))")
        }
        let inputFormat = input.inputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw CaptureError.microphoneFailed("microphone format is unavailable")
        }

        format = Format(sampleRate: inputFormat.sampleRate, channels: 1)
        stateBox.updateFormat(sampleRate: inputFormat.sampleRate, channels: 1)

        input.installTap(onBus: 0, bufferSize: 1024, format: inputFormat) {
            [stateBox] buffer, time in
            guard buffer.frameLength > 0, let channel = buffer.floatChannelData?[0] else { return }
            stateBox.emit(channel, frameCount: buffer.frameLength, channels: 1,
                          hostTime: time.isHostTimeValid ? time.hostTime : nil)
        }
        tapped = true

        do {
            engine.prepare()
            try engine.start()
        } catch {
            input.removeTap(onBus: 0)
            tapped = false
            throw CaptureError.microphoneFailed(error.localizedDescription)
        }
        self.engine = engine
        var actual = selected.id
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        if AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                kAudioUnitScope_Global, 0, &actual, &size) != noErr {
            actual = selected.id
        }
        deviceName = resolver.name(actual)
        let boundDevice = MicrophoneDevice(id: actual, name: deviceName)
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange,
            object: engine,
            queue: nil
        ) { [weak self] _ in
            guard let self else { return }
            self.onConfigurationChange(self.identifier, MREC_TRACK_REASON_DEVICE_CHANGED,
                                       "microphone configuration changed")
        }
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            if resolver.resolve(source: source, pids: liveTargets()) != boundDevice {
                self.onConfigurationChange(self.identifier, MREC_TRACK_REASON_DEVICE_CHANGED,
                                           "microphone device changed")
            }
        }
        timer.resume()
        deviceTimer = timer
    }

    func stop() {
        deviceTimer?.cancel()
        deviceTimer = nil
        if let configurationObserver {
            NotificationCenter.default.removeObserver(configurationObserver)
            self.configurationObserver = nil
        }
        guard let engine else { return }
        if tapped {
            engine.inputNode.removeTap(onBus: 0)
            tapped = false
        }
        engine.stop()
        self.engine = nil
    }
}
