import CoreAudio
import Foundation

/// Callback signature matching `mrec_audio_callback` in meeting-record.h.
public typealias MrecAudioCallback = @convention(c) (
    UnsafePointer<Float>?, UInt32, UInt32, Double, UInt64, UnsafeMutableRawPointer?
) -> Void

/// Holds the host's callback so the realtime IOProc can reach it through a raw
/// pointer. A class because it is passed as IOProc `clientData`.
///
/// `emit` reads only fields that are immutable after start, so it takes no lock.
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
        guard let callback else { return }
        let hostTimeNs = nanoseconds(hostTime ?? mach_absolute_time())
        callback(frames, frameCount, channels, sampleRate, hostTimeNs, userData)
    }

    /// Interleave a potentially non-interleaved HAL buffer list without allocating
    /// on the realtime thread. CoreAudio taps may expose stereo as two mono buffers.
    @inline(__always)
    func emit(_ buffers: UnsafeMutableAudioBufferListPointer, hostTime: UInt64?) {
        guard !buffers.isEmpty else { return }
        if buffers.count == 1, let data = buffers[0].mData,
           buffers[0].mDataByteSize > 0 {
            let bufferChannels = max(buffers[0].mNumberChannels, 1)
            let frameCount = buffers[0].mDataByteSize / 4 / bufferChannels
            emit(data.assumingMemoryBound(to: Float.self), frameCount: frameCount,
                 channels: bufferChannels, hostTime: hostTime)
            return
        }

        let totalChannels = buffers.reduce(UInt32(0)) {
            $0 + max($1.mNumberChannels, 1)
        }
        guard totalChannels > 0, totalChannels == channels,
              let scratch = interleaveBuffer, interleaveCapacity >= Int(totalChannels) else {
            return
        }

        var frameCount = UInt32.max
        for buffer in buffers {
            guard buffer.mData != nil, buffer.mDataByteSize > 0 else { return }
            frameCount = min(frameCount,
                             buffer.mDataByteSize / 4 / max(buffer.mNumberChannels, 1))
        }
        guard frameCount > 0, frameCount != UInt32.max else { return }

        let framesPerPass = max(1, interleaveCapacity / Int(totalChannels))
        var frameOffset = 0
        while frameOffset < Int(frameCount) {
            let passFrames = min(framesPerPass, Int(frameCount) - frameOffset)
            var destinationChannel = 0
            for buffer in buffers {
                guard let raw = buffer.mData else { return }
                let source = raw.assumingMemoryBound(to: Float.self)
                let sourceChannels = Int(max(buffer.mNumberChannels, 1))
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
