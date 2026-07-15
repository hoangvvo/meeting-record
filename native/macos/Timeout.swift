import Foundation

/// The worker may outlive the `withTimeout` call, so the result cannot live on
/// that stack frame.
private final class ResultBox<T> {
    var value: T?
}

/// Run `work` on a detached thread and give up after `seconds`.
///
/// Starting a tap-backed aggregate device blocks indefinitely while the
/// audio-capture TCC grant is undetermined: `AudioDeviceCreateIOProcID` waits on
/// coreaudiod, which waits on tccd, which waits for a prompt only a GUI process
/// can present.
///
/// On timeout the worker stays blocked in the kernel and cannot be cancelled; it
/// unblocks once TCC resolves, and the OS reclaims anything it holds at process
/// exit. Treat a timeout as undetermined rather than denied.
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
