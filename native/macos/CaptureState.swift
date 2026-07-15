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

    func configure(callback: MrecAudioCallback?, userData: UnsafeMutableRawPointer?) {
        self.callback = callback
        self.userData = userData
    }

    func updateFormat(sampleRate: Double, channels: UInt32) {
        self.sampleRate = sampleRate
        self.channels = channels
    }

    func currentFormat() -> (sampleRate: Double, channels: UInt32) {
        (sampleRate, channels)
    }

    func clear() {
        callback = nil
        userData = nil
        sampleRate = 0
        channels = 0
    }

    /// Called on the realtime IOProc thread.
    @inline(__always)
    func emit(_ frames: UnsafePointer<Float>, frameCount: UInt32, channels: UInt32) {
        guard let callback else { return }
        let hostTimeNs = mach_absolute_time() * timebaseNumer / timebaseDenom
        callback(frames, frameCount, channels, sampleRate, hostTimeNs, userData)
    }
}
