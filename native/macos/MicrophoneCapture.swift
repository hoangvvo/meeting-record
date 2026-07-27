import AVFAudio
import AVFoundation
import Foundation

final class MicrophoneCapture {
    struct Format {
        var sampleRate: Double
        var channels: UInt32
    }

    private(set) var format = Format(sampleRate: 0, channels: 0)

    private let stateBox: CaptureStateBox
    let identifier = UUID()
    private let onConfigurationChange: (UUID, String) -> Void
    private var engine: AVAudioEngine?
    private var tapped = false
    private var configurationObserver: NSObjectProtocol?

    init(stateBox: CaptureStateBox,
         onConfigurationChange: @escaping (UUID, String) -> Void = { _, _ in }) {
        self.stateBox = stateBox
        self.onConfigurationChange = onConfigurationChange
    }

    func start() throws {
        guard Permissions.microphoneStatus() == 1 else {
            throw CaptureError.microphonePermission
        }

        let engine = AVAudioEngine()
        let input = engine.inputNode
        let inputFormat = input.inputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw CaptureError.microphoneFailed("no default microphone is available")
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
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange,
            object: engine,
            queue: nil
        ) { [weak self] _ in
            guard let self else { return }
            self.onConfigurationChange(self.identifier, "default input device changed")
        }
    }

    func stop() {
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
