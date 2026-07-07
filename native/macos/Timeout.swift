import Foundation

/// Result holder: the worker thread may outlive the `withTimeout` call, so its
/// result cannot live on that stack frame.
private final class ResultBox<T> {
    var value: T?
}

/// Run `work` on a detached thread and give up after `seconds`.
///
/// This exists because the CoreAudio calls that start a tap-backed aggregate
/// device **block indefinitely** when the audio-capture TCC grant is still
/// undetermined: `AudioDeviceCreateIOProcID` sits in a mach round-trip to
/// coreaudiod, which is waiting on tccd, which is waiting for a user prompt that
/// only a GUI process can present. A library must never hang its host on that.
///
/// On timeout the worker thread is left blocked — it is stuck in the kernel and
/// cannot be safely cancelled. It unblocks on its own once TCC resolves, and any
/// tap/aggregate it holds is reclaimed by the OS at process exit. Callers should
/// treat a timeout as "undetermined", not "denied", and surface the permission
/// flow to the user instead of retrying in a loop.
func withTimeout<T>(seconds: TimeInterval, _ work: @escaping () -> T) -> T? {
    let semaphore = DispatchSemaphore(value: 0)
    let box = ResultBox<T>()

    Thread.detachNewThread {
        let value = work()
        box.value = value
        semaphore.signal()
    }

    return semaphore.wait(timeout: .now() + seconds) == .success ? box.value : nil
}
