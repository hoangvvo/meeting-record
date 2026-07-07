import AppKit
import CoreAudio
import Foundation

/// Watches for meetings starting, changing and ending.
///
/// Event-driven rather than a poll loop. Three triggers, cheapest first:
///
///  * `NSWorkspace` launch/terminate notifications — an app appearing or going
///    away can only change the answer.
///  * A CoreAudio listener on `kAudioHardwarePropertyProcessObjectList` — fires
///    when any process starts or stops doing audio IO, which is exactly the
///    transition that turns "Zoom is open" into "Zoom is in a call".
///  * A slow backstop timer, because neither of the above fires when a call's
///    state changes without the process set changing (someone unmutes, the tab
///    navigates to a different meeting).
final class MeetingWatcher {
    /// Slow enough not to matter, fast enough that a mute toggle shows up.
    private static let backstopInterval: TimeInterval = 5.0
    /// Audio process lists churn in bursts; let them settle before re-scanning.
    private static let debounceInterval: TimeInterval = 0.4

    private let queue = DispatchQueue(label: "meetingrecord.watcher")
    private let callback: MrecMeetingCallback
    private let userData: UnsafeMutableRawPointer?

    /// Last reported state, keyed by pid, for diffing.
    private var active: [pid_t: DetectedMeeting] = [:]
    private var observers: [NSObjectProtocol] = []
    private var timer: DispatchSourceTimer?
    private var debounce: DispatchWorkItem?
    private var listenerBlock: AudioObjectPropertyListenerBlock?

    init(callback: MrecMeetingCallback, userData: UnsafeMutableRawPointer?) {
        self.callback = callback
        self.userData = userData
    }

    func start() {
        let center = NSWorkspace.shared.notificationCenter
        for name in [NSWorkspace.didLaunchApplicationNotification,
                     NSWorkspace.didTerminateApplicationNotification] {
            observers.append(center.addObserver(forName: name, object: nil,
                                               queue: nil) { [weak self] _ in
                self?.scheduleRescan()
            })
        }

        // Process-list changes are the signal that a call went live.
        var address = HAL.address(kAudioHardwarePropertyProcessObjectList)
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            self?.scheduleRescan()
        }
        listenerBlock = block
        AudioObjectAddPropertyListenerBlock(HAL.system, &address, queue, block)

        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + Self.backstopInterval,
                       repeating: Self.backstopInterval)
        timer.setEventHandler { [weak self] in self?.rescan() }
        timer.resume()
        self.timer = timer

        // Emit current state immediately so callers do not wait for a transition.
        queue.async { [weak self] in self?.rescan() }
    }

    func stop() {
        let center = NSWorkspace.shared.notificationCenter
        observers.forEach(center.removeObserver)
        observers.removeAll()

        timer?.cancel()
        timer = nil
        debounce?.cancel()
        debounce = nil

        if let block = listenerBlock {
            var address = HAL.address(kAudioHardwarePropertyProcessObjectList)
            AudioObjectRemovePropertyListenerBlock(HAL.system, &address, queue, block)
            listenerBlock = nil
        }
        // Deliberately not synthesising ENDED events here: the caller asked to stop
        // watching, so it is not waiting for teardown notifications.
        queue.sync { active.removeAll() }
    }

    // MARK: - Scanning

    private func scheduleRescan() {
        debounce?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.rescan() }
        debounce = work
        queue.asyncAfter(deadline: .now() + Self.debounceInterval, execute: work)
    }

    private func rescan() {
        let found = MeetingDetector.scan()
        var next: [pid_t: DetectedMeeting] = [:]
        for meeting in found { next[meeting.pid] = meeting }

        for (pid, meeting) in next {
            if let previous = active[pid] {
                if changed(previous, meeting) { emit(meeting, .updated) }
            } else {
                emit(meeting, .started)
            }
        }
        for (pid, meeting) in active where next[pid] == nil {
            emit(meeting, .ended)
        }
        active = next
    }

    /// Only the fields a consumer would act on — not confidence jitter.
    private func changed(_ a: DetectedMeeting, _ b: DetectedMeeting) -> Bool {
        a.platform != b.platform
            || a.title != b.title
            || a.url != b.url
            || a.isUsingMic != b.isUsingMic
            || a.isPlayingAudio != b.isPlayingAudio
            || a.audioPIDs != b.audioPIDs
    }

    private func emit(_ meeting: DetectedMeeting, _ event: MrecMeetingEvent) {
        withCMeeting(meeting) { pointer in
            callback(pointer, event.rawValue, userData)
        }
    }
}

enum MrecMeetingEvent: Int32 {
    case started = 0
    case updated = 1
    case ended = 2
}
