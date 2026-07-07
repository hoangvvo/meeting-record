import CoreAudio
import Foundation

/// Callback signature matching `mrec_audio_callback` in meeting-record.h.
public typealias MrecAudioCallback = @convention(c) (
    UnsafePointer<Float>?, UInt32, UInt32, Double, UInt64, UnsafeMutableRawPointer?
) -> Void

/// Holds the host's callback so the realtime IOProc can reach it through a raw
/// pointer. Reference type because it is passed as IOProc `clientData`.
///
/// The realtime path (`emit`) only reads immutable-after-start fields, so no
/// locking is needed there — which matters, because taking a lock on a CoreAudio
/// IO thread risks priority inversion and dropouts.
final class CaptureStateBox {
    private var callback: MrecAudioCallback?
    private var userData: UnsafeMutableRawPointer?

    private var sampleRate: Double = 0
    private var channels: UInt32 = 0

    /// Host clock ticks -> nanoseconds. Resolved once; `mach_timebase_info` is a
    /// syscall-free read but we still avoid it on the audio thread.
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

    /// Called from the realtime IOProc.
    @inline(__always)
    func emit(_ frames: UnsafePointer<Float>, frameCount: UInt32, channels: UInt32) {
        guard let callback else { return }
        let hostTimeNs = mach_absolute_time() * timebaseNumer / timebaseDenom
        callback(frames, frameCount, channels, sampleRate, hostTimeNs, userData)
    }
}
