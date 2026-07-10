/**
 * meeting-record — system-audio capture and meeting detection.
 *
 * The addon is deliberately not re-exported. Everything below wraps it so that
 * callers never see pids, status codes, or the realtime callback.
 */
import { EventEmitter } from 'node:events'
import { Readable } from 'node:stream'
import { createRequire } from 'node:module'

const require_ = createRequire(__filename)
const native = require_('../build/Release/meeting_record.node')

export type Permission = 'system-audio' | 'accessibility'
export type PermissionStatus = 'granted' | 'denied' | 'unknown' | 'not-required'

export type Platform =
  | 'Zoom'
  | 'Microsoft Teams'
  | 'Google Meet'
  | 'Webex'
  | 'Slack'
  | 'Discord'
  | 'Browser call'
  | 'Unknown'

export interface Meeting {
  readonly platform: Platform
  /** The application the user sees. Not necessarily where the audio comes from. */
  readonly pid: number
  readonly appName: string
  /** Requires the accessibility permission; empty string otherwise. */
  readonly title: string
  /** Requires the accessibility permission; empty string otherwise. */
  readonly url: string
  readonly isUsingMic: boolean
  readonly isPlayingAudio: boolean
  readonly confidence: number
  /** Prefer this over comparing `confidence`; the weights may change. */
  readonly shouldRecord: boolean
}

export interface AudioProcess {
  readonly pid: number
  readonly name: string
  readonly bundleId: string
  readonly isPlayingAudio: boolean
  readonly isUsingMic: boolean
}

export interface CaptureOptions {
  /** Record the processes this meeting is using. The usual choice. */
  meeting?: Meeting
  /** Or specific processes. */
  pids?: number[]
  /** Or the whole system mix. Records silence while the user's output is muted. */
  systemWide?: boolean
  /** Default true. */
  mono?: boolean
}

export class MeetingRecordError extends Error {
  constructor(message: string, readonly code: number) {
    super(message)
    this.name = 'MeetingRecordError'
  }
}

/* ---- permissions ------------------------------------------------------- */

export const permissions = {
  status(permission: Permission): PermissionStatus {
    return permission === 'accessibility'
      ? native.accessibilityPermissionStatus()
      : native.audioPermissionStatus()
  },

  /**
   * Show the permission prompt and resolve once the user answers.
   *
   * System audio shows a dialog; accessibility opens System Settings and requires
   * an app relaunch, so it resolves as soon as Settings is open rather than
   * waiting for a grant that cannot arrive in this process.
   */
  async request(
    permission: Permission,
    { timeoutMs = 120_000 }: { timeoutMs?: number } = {},
  ): Promise<PermissionStatus> {
    const current = permissions.status(permission)
    if (current === 'granted' || current === 'not-required') return current

    if (permission === 'accessibility') {
      native.requestAccessibilityPermission()
      return permissions.status(permission)
    }

    native.requestAudioPermission()

    // The dialog is modal to the user, not to us, so poll for the answer.
    const deadline = Date.now() + timeoutMs
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 500))
      const status = permissions.status(permission)
      if (status === 'granted' || status === 'denied') return status
    }
    return permissions.status(permission)
  },
}

/* ---- meeting detection ------------------------------------------------- */

/** Hidden from the public `Meeting` type: pids are unstable, so callers should
 *  pass the meeting object back rather than reading them. */
const audioPidsOf = new WeakMap<Meeting, number[]>()

function toMeeting(raw: any): Meeting {
  const { _audioPids, platformId, ...rest } = raw
  const meeting = rest as Meeting
  Object.defineProperty(meeting, '__platformId', { value: platformId, enumerable: false })
  audioPidsOf.set(meeting, _audioPids ?? [])
  return meeting
}

function toNative(meeting: Meeting): any {
  return {
    platformId: (meeting as any).__platformId ?? 0,
    pid: meeting.pid,
    _audioPids: audioPidsOf.get(meeting) ?? [],
  }
}

export interface MeetingEvents {
  started: (meeting: Meeting) => void
  updated: (meeting: Meeting) => void
  ended: (meeting: Meeting) => void
}

class Meetings extends EventEmitter {
  /** Point-in-time scan, highest confidence first. */
  scan(): Meeting[] {
    return native.scan().map(toMeeting)
  }

  /** Begin emitting `started` / `updated` / `ended`. */
  watch(): void {
    if (native.isWatching()) return
    const status = native.watchStart((event: keyof MeetingEvents, raw: any) => {
      this.emit(event, toMeeting(raw))
    })
    if (status !== 0) throw new MeetingRecordError('failed to start watching', status)
  }

  unwatch(): void {
    if (native.isWatching()) native.watchStop()
  }

  get watching(): boolean {
    return native.isWatching()
  }

  /** Every process doing audio IO. Rarely needed; `scan()` is the normal path. */
  audioProcesses(): AudioProcess[] {
    return native.listAudioProcesses()
  }
}

export const meetings = new Meetings() as Meetings & {
  on<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings
  once<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings
  off<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings
}

/* ---- capture ----------------------------------------------------------- */

/**
 * Interleaved float32 PCM.
 *
 * A stream rather than an event, because the underlying callback runs on a
 * realtime thread and will not wait for a slow consumer. Backpressure is real:
 * if this stream is not read, the native ring buffer overwrites its oldest
 * samples and `drop` fires.
 */
export class CaptureSession extends Readable {
  constructor(
    readonly sampleRate: number,
    readonly channels: number,
  ) {
    super({ objectMode: false, highWaterMark: 1 << 20 })
  }

  /** Required by Readable; data arrives from the native side, not on demand. */
  _read(): void {}

  async stop(): Promise<void> {
    native.captureStop()
    this.push(null)
  }
}

export const capture = {
  async start(options: CaptureOptions = {}): Promise<CaptureSession> {
    let session: CaptureSession | undefined
    let pendingDrops = 0

    const onData = (chunk: Buffer, info: { dropped: number }) => {
      if (info.dropped > 0) {
        pendingDrops += info.dropped
        session?.emit('drop', info.dropped)
      }
      // Backpressure is advisory here: the native ring buffer, not this stream,
      // is what actually absorbs a slow consumer.
      session?.push(chunk)
    }

    const nativeOptions = {
      ...options,
      meeting: options.meeting ? toNative(options.meeting) : undefined,
    }

    try {
      const format = await native.captureStart(nativeOptions, onData)
      session = new CaptureSession(format.sampleRate, format.channels)
      if (pendingDrops > 0) session.emit('drop', pendingDrops)
      return session
    } catch (error: any) {
      throw new MeetingRecordError(error.message ?? String(error), error.code ?? -9)
    }
  },

  get running(): boolean {
    return native.isRunning()
  },
}

/* ---- convenience ------------------------------------------------------- */

export interface AutoRecordOptions {
  /** Called when a meeting worth recording starts. */
  onStart: (session: CaptureSession, meeting: Meeting) => void
  onStop?: (meeting: Meeting) => void
  onError?: (error: Error, meeting: Meeting) => void
  /** Override the library's own judgement. */
  shouldRecord?: (meeting: Meeting) => boolean
}

/**
 * Record every meeting automatically. The common case, in one call.
 */
export function autoRecord(options: AutoRecordOptions): { stop(): void } {
  const active = new Map<number, Meeting>()

  const onStarted = async (meeting: Meeting) => {
    const wanted = options.shouldRecord?.(meeting) ?? meeting.shouldRecord
    if (!wanted || capture.running) return
    try {
      const session = await capture.start({ meeting })
      active.set(meeting.pid, meeting)
      options.onStart(session, meeting)
    } catch (error) {
      options.onError?.(error as Error, meeting)
    }
  }

  const onEnded = async (meeting: Meeting) => {
    if (!active.delete(meeting.pid)) return
    if (capture.running) native.captureStop()
    options.onStop?.(meeting)
  }

  meetings.on('started', onStarted)
  meetings.on('ended', onEnded)
  meetings.watch()

  return {
    stop() {
      meetings.off('started', onStarted)
      meetings.off('ended', onEnded)
      meetings.unwatch()
      if (capture.running) native.captureStop()
    },
  }
}

export default { permissions, meetings, capture, autoRecord }
