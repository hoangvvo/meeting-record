import Foundation

/// The worker may outlive the `withTimeout` call, so the result cannot live on
/// that stack frame.
private final class ResultBox<T> {
    var value: T?
}

/// Return after `seconds` even if work is still blocked. The worker cannot be cancelled.
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
