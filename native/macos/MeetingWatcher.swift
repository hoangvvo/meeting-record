import AppKit
import CoreAudio
import Foundation

/// Rescan on app/audio changes, with a timer for changes those notifications miss.
final class MeetingWatcher {
    /// Backstop interval for changes the other triggers miss.
    private static let backstopInterval: TimeInterval = 5.0
    /// Audio process lists churn in bursts, so let them settle before re-scanning.
    private static let debounceInterval: TimeInterval = 0.4
    /// Shared with the Windows watcher through meeting-catalog.h.
    private static let startDelay = TimeInterval(MREC_CATALOG_START_DELAY_MS) / 1000
    private static let endDelay = TimeInterval(MREC_CATALOG_END_DELAY_MS) / 1000

    private struct Tracked {
        var meeting: DetectedMeeting
        let firstSeen: TimeInterval
        var lastSeen: TimeInterval
        var started = false
    }

    private let queue = DispatchQueue(label: "meetingrecord.watcher")
    private let queueKey = DispatchSpecificKey<UInt8>()
    private let callback: MrecMeetingCallback
    private let userData: UnsafeMutableRawPointer?

    /// Meetings seen so far, keyed by pid, for diffing.
    private var tracked: [pid_t: Tracked] = [:]
    private var observers: [NSObjectProtocol] = []
    private var timer: DispatchSourceTimer?
    private var debounce: DispatchWorkItem?
    private var followUp: DispatchWorkItem?
    private var listenerBlock: AudioObjectPropertyListenerBlock?
    private var stopped = false

    init(callback: MrecMeetingCallback, userData: UnsafeMutableRawPointer?) {
        self.callback = callback
        self.userData = userData
        queue.setSpecific(key: queueKey, value: 1)
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

        // Process-list changes indicate a call going live.
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

        // Emit current state so callers need not wait for a transition.
        queue.async { [weak self] in self?.rescan() }
    }

    func stop() {
        let center = NSWorkspace.shared.notificationCenter
        observers.forEach(center.removeObserver)
        observers.removeAll()

        if let block = listenerBlock {
            var address = HAL.address(kAudioHardwarePropertyProcessObjectList)
            AudioObjectRemovePropertyListenerBlock(HAL.system, &address, queue, block)
            listenerBlock = nil
        }
        let cleanup = {
            self.stopped = true
            self.timer?.cancel()
            self.timer = nil
            self.debounce?.cancel()
            self.debounce = nil
            self.followUp?.cancel()
            self.followUp = nil
            // No ENDED events on explicit stop.
            self.tracked.removeAll()
        }
        if DispatchQueue.getSpecific(key: self.queueKey) != nil {
            cleanup()
        } else {
            self.queue.sync(execute: cleanup)
        }
    }

    // MARK: - Scanning

    private func scheduleRescan() {
        queue.async { [weak self] in
            guard let self else { return }
            guard !self.stopped else { return }
            self.debounce?.cancel()
            let work = DispatchWorkItem { [weak self] in self?.rescan() }
            self.debounce = work
            self.queue.asyncAfter(deadline: .now() + Self.debounceInterval,
                                  execute: work)
        }
    }

    private func rescan() {
        guard !stopped else { return }
        let now = ProcessInfo.processInfo.systemUptime

        for meeting in MeetingDetector.scan() {
            guard !stopped else { return }
            var entry = tracked[meeting.pid]
                ?? Tracked(meeting: meeting, firstSeen: now, lastSeen: now)
            let updated = entry.started && changed(entry.meeting, meeting)
            let starting = !entry.started && now - entry.firstSeen >= Self.startDelay
            entry.meeting = meeting
            entry.lastSeen = now
            entry.started = entry.started || starting
            tracked[meeting.pid] = entry
            if starting {
                emit(meeting, .started)
            } else if updated {
                emit(meeting, .updated)
            }
        }
        for (pid, entry) in tracked where entry.lastSeen < now {
            guard !stopped else { return }
            if entry.started && now - entry.lastSeen < Self.endDelay { continue }
            tracked[pid] = nil
            if entry.started { emit(entry.meeting, .ended) }
        }
        guard !stopped else { return }
        scheduleFollowUp(now: now)
    }

    /// Settles pending starts and endings without waiting for the backstop.
    private func scheduleFollowUp(now: TimeInterval) {
        followUp?.cancel()
        followUp = nil
        let deadlines = tracked.values.compactMap { entry -> TimeInterval? in
            if !entry.started { return entry.firstSeen + Self.startDelay }
            return entry.lastSeen < now ? entry.lastSeen + Self.endDelay : nil
        }
        guard let deadline = deadlines.min() else { return }
        let work = DispatchWorkItem { [weak self] in self?.rescan() }
        followUp = work
        queue.asyncAfter(deadline: .now() + max(deadline - now, 0), execute: work)
    }

    /// Compares only fields a consumer would act on, excluding confidence.
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
