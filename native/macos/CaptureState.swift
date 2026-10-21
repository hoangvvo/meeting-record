import CoreAudio
import Foundation

/// Callback signature matching `mrec_audio_callback` in meeting-record.h.
public typealias MrecAudioCallback = @convention(c) (
    UnsafePointer<Float>?, UInt32, UInt32, Double, UInt64, UnsafeMutableRawPointer?
) -> Void

/// Callback state passed through IOProc clientData. Immutable while recording.
final class CaptureStateBox {
    private var callback: MrecAudioCallback?
    private var userData: UnsafeMutableRawPointer?

    private var sampleRate: Double = 0
    private var channels: UInt32 = 0
    private var interleaveBuffer: UnsafeMutablePointer<Float>?
    private var interleaveCapacity = 0

    /// Host clock ticks -> nanoseconds, resolved once to keep it off the audio
    /// thread.
    private var timebaseNumer: UInt64 = 1
    private var timebaseDenom: UInt64 = 1

    init() {
        var info = mach_timebase_info_data_t()
        if mach_timebase_info(&info) == KERN_SUCCESS, info.denom != 0 {
            timebaseNumer = UInt64(info.numer)
            timebaseDenom = UInt64(info.denom)
        }
    }

    deinit {
        interleaveBuffer?.deallocate()
    }

    func configure(callback: MrecAudioCallback?, userData: UnsafeMutableRawPointer?) {
        self.callback = callback
        self.userData = userData
    }

    func updateFormat(sampleRate: Double, channels: UInt32,
                      maximumFrameCount: UInt32 = 0) {
        self.sampleRate = sampleRate
        self.channels = channels
        interleaveBuffer?.deallocate()
        interleaveBuffer = nil
        interleaveCapacity = 0
        if channels > 1, maximumFrameCount > 0 {
            let capacity = Int(channels) * Int(maximumFrameCount)
            interleaveBuffer = UnsafeMutablePointer<Float>.allocate(capacity: capacity)
            interleaveCapacity = capacity
        }
    }

    func currentFormat() -> (sampleRate: Double, channels: UInt32) {
        (sampleRate, channels)
    }

    func clear() {
        callback = nil
        userData = nil
        sampleRate = 0
        channels = 0
        interleaveBuffer?.deallocate()
        interleaveBuffer = nil
        interleaveCapacity = 0
    }

    /// Called on the realtime IOProc thread.
    @inline(__always)
    func emit(_ frames: UnsafePointer<Float>, frameCount: UInt32, channels: UInt32,
              hostTime: UInt64? = nil) {
        guard let callback, frameCount > 0, channels > 0, channels == self.channels,
              sampleRate.isFinite, sampleRate > 0 else { return }
        let hostTimeNs = nanoseconds(hostTime ?? mach_absolute_time())
        callback(frames, frameCount, channels, sampleRate, hostTimeNs, userData)
    }

    /// Interleave a potentially non-interleaved HAL buffer list without allocating
    /// on the realtime thread. CoreAudio taps may expose stereo as two mono buffers.
    @inline(__always)
    func emit(_ buffers: UnsafeMutableAudioBufferListPointer, hostTime: UInt64?) {
        guard !buffers.isEmpty else { return }
        if buffers.count == 1 {
            let buffer = buffers[0]
            let bufferChannels = buffer.mNumberChannels
            guard let data = buffer.mData, buffer.mDataByteSize > 0,
                  bufferChannels > 0, bufferChannels == channels,
                  buffer.mDataByteSize % 4 == 0,
                  (buffer.mDataByteSize / 4) % bufferChannels == 0 else { return }
            let frameCount = buffer.mDataByteSize / 4 / bufferChannels
            emit(data.assumingMemoryBound(to: Float.self), frameCount: frameCount,
                 channels: bufferChannels, hostTime: hostTime)
            return
        }

        var totalChannels: UInt32 = 0
        var frameCount: UInt32 = 0
        for buffer in buffers {
            let count = buffer.mNumberChannels
            guard buffer.mData != nil, buffer.mDataByteSize > 0,
                  count > 0, count <= channels - totalChannels,
                  buffer.mDataByteSize % 4 == 0,
                  (buffer.mDataByteSize / 4) % count == 0 else { return }
            let frames = buffer.mDataByteSize / 4 / count
            guard frameCount == 0 || frameCount == frames else { return }
            frameCount = frames
            totalChannels += count
        }
        guard frameCount > 0, totalChannels == channels,
              let scratch = interleaveBuffer, interleaveCapacity >= Int(totalChannels) else { return }

        let framesPerPass = max(1, interleaveCapacity / Int(totalChannels))
        var frameOffset = 0
        while frameOffset < Int(frameCount) {
            let passFrames = min(framesPerPass, Int(frameCount) - frameOffset)
            var destinationChannel = 0
            for buffer in buffers {
                guard let raw = buffer.mData else { return }
                let source = raw.assumingMemoryBound(to: Float.self)
                let sourceChannels = Int(buffer.mNumberChannels)
                for frame in 0..<passFrames {
                    for channel in 0..<sourceChannels {
                        scratch[frame * Int(totalChannels) + destinationChannel + channel] =
                            source[(frameOffset + frame) * sourceChannels + channel]
                    }
                }
                destinationChannel += sourceChannels
            }
            let passHostTime: UInt64?
            if let hostTime, sampleRate > 0 {
                let elapsedNs = UInt64(Double(frameOffset) / sampleRate * 1_000_000_000)
                passHostTime = hostTime + nanosecondsToHostTime(elapsedNs)
            } else {
                passHostTime = hostTime
            }
            emit(UnsafePointer(scratch), frameCount: UInt32(passFrames),
                 channels: totalChannels, hostTime: passHostTime)
            frameOffset += passFrames
        }
    }

    @inline(__always)
    private func nanoseconds(_ hostTime: UInt64) -> UInt64 {
        (hostTime / timebaseDenom) * timebaseNumer
            + (hostTime % timebaseDenom) * timebaseNumer / timebaseDenom
    }

    @inline(__always)
    private func nanosecondsToHostTime(_ nanoseconds: UInt64) -> UInt64 {
        (nanoseconds / timebaseNumer) * timebaseDenom
            + (nanoseconds % timebaseNumer) * timebaseDenom / timebaseNumer
    }
}
