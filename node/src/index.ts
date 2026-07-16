/**
 * meeting-record — system-audio and microphone capture with meeting detection.
 *
 * Wraps the native addon so callers never see helper-process pids, status codes,
 * or the realtime callback. The addon itself is not re-exported.
 */
import { EventEmitter } from 'node:events'
import { Readable } from 'node:stream'
import { createRequire } from 'node:module'

// Native addons cannot be imported under ESM. Resolved relative to dist/index.js.
const native = createRequire(import.meta.url)('../build/Release/meeting_record.node')

export type Permission = 'system-audio' | 'microphone' | 'accessibility'
export type PermissionStatus = 'granted' | 'denied' | 'unknown' | 'not-required'

/**
 * Stable platform identifier; the values never change. No display labels are
 * provided — presentation and localisation belong to the caller.
 */
export type Platform =
  | 'zoom'
  | 'teams'
  | 'meet'
  | 'webex'
  | 'slack'
  | 'discord'
  /** A browser tab on a recognised call URL. */
  | 'browser'
  | 'unknown'

export interface Meeting {
  readonly platform: Platform
  /** The visible app. Use it in `CaptureTarget` when starting capture. */
  readonly pid: number
  readonly appName: string
  /** Requires the accessibility permission; empty string otherwise. */
  readonly title: string
  /** Requires the accessibility permission; empty string otherwise. */
  readonly url: string
  readonly isUsingMic: boolean
  readonly isPlayingAudio: boolean
  readonly confidence: number
  /** Prefer this to comparing `confidence`; the weights may change. */
  readonly shouldRecord: boolean
}

export interface AudioProcess {
  readonly pid: number
  readonly name: string
  readonly bundleId: string
  readonly isPlayingAudio: boolean
  readonly isUsingMic: boolean
}

export type CaptureTarget =
  | {
      /** A detected meeting's app pid, or any audio-producing process pid. */
      type: 'process'
      pid: number
    }
  | {
      /** The whole system mix. Records silence while output is muted. */
      type: 'system'
    }

export type MicrophoneSource = 'default'

export interface CaptureOptions {
  /** Mono mixdown for system audio. Default true. */
  mono?: boolean
  /** Capture the default microphone as a separate track. Default off. */
  microphone?: MicrophoneSource
}

export class MeetingRecordError extends Error {
  constructor(message: string, readonly code: number) {
    super(message)
    this.name = new.target.name
  }
}

export class UnsupportedOsError extends MeetingRecordError {}
export class PermissionError extends MeetingRecordError {}
export class AlreadyRunningError extends MeetingRecordError {}
export class CaptureError extends MeetingRecordError {}
export class InternalError extends MeetingRecordError {}

function errorFromStatus(message: string, code: number): MeetingRecordError {
  switch (code) {
    case -1:
      return new UnsupportedOsError(message, code)
    case -2:
      return new PermissionError(message, code)
    case -3:
      return new AlreadyRunningError(message, code)
    case -4:
    case -5:
    case -6:
    case -7:
    case -8:
      return new CaptureError(message, code)
    default:
      return new InternalError(message, code)
  }
}

function asLibraryError(error: unknown): Error {
  if (error instanceof TypeError) return error
  const message = error instanceof Error ? error.message : String(error)
  const code = (error as { code?: unknown } | null)?.code
  return typeof code === 'number' ? errorFromStatus(message, code) : new InternalError(message, -9)
}

export const permissions = {
  status(permission: Permission): PermissionStatus {
    if (
      permission !== 'system-audio' &&
      permission !== 'microphone' &&
      permission !== 'accessibility'
    ) {
      throw new TypeError(`unknown permission: ${String(permission)}`)
    }
    if (permission === 'accessibility') return native.accessibilityPermissionStatus()
    if (permission === 'microphone') return native.microphonePermissionStatus()
    return native.audioPermissionStatus()
  },

  /**
   * Show the permission prompt and resolve once the user answers.
   *
   * Accessibility opens System Settings and needs an app relaunch, so it resolves
   * as soon as Settings is open.
   */
  async request(
    permission: Permission,
    { timeoutMs = 120_000 }: { timeoutMs?: number } = {},
  ): Promise<PermissionStatus> {
    const current = permissions.status(permission)
    if (current === 'granted' || current === 'not-required') return current

    if (permission === 'accessibility') {
      const status = native.requestAccessibilityPermission()
      if (status !== 0) throw errorFromStatus('failed to request accessibility permission', status)
      return permissions.status(permission)
    }

    const requestStatus =
      permission === 'microphone'
        ? native.requestMicrophonePermission()
        : native.requestAudioPermission()
    if (requestStatus !== 0) {
      throw errorFromStatus(`failed to request ${permission} permission`, requestStatus)
    }

    // The dialog is modal to the user, not to this process.
    const deadline = Date.now() + timeoutMs
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 500))
      const status = permissions.status(permission)
      if (status === 'granted' || status === 'denied') return status
    }
    return permissions.status(permission)
  },
}

function toMeeting(raw: any): Meeting {
  return raw as Meeting
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
    if (status !== 0) throw errorFromStatus('failed to start watching', status)
  }

  unwatch(): void {
    if (native.isWatching()) native.watchStop()
  }

  get watching(): boolean {
    return native.isWatching()
  }

  /** Every process doing audio IO. */
  audioProcesses(): AudioProcess[] {
    return native.listAudioProcesses()
  }
}

export const meetings = new Meetings() as Meetings & {
  on<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings
  once<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings
  off<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings
}

/**
 * Interleaved float32 PCM.
 *
 * The native callback never waits for a slow consumer. If this stream is not read,
 * its preallocated ring buffer drops samples and emits `drop`.
 */
export type CaptureState = 'recording' | 'paused' | 'stopped'

export interface AudioTrack extends Readable {
  readonly sampleRate: number
  readonly channels: number
  /** Interleaved samples discarded because the bounded queue could not accept them. */
  readonly droppedSamples: number
}

export interface CaptureSession {
  readonly systemAudio: AudioTrack
  readonly microphone?: AudioTrack
  readonly state: CaptureState
  pauseRecording(): void
  resumeRecording(): void
  stopRecording(): Promise<void>
}

class AudioTrackImpl extends Readable implements AudioTrack {
  readonly #sampleRate: number
  readonly #channels: number
  #droppedSamples = 0

  constructor(sampleRate: number, channels: number) {
    super({ objectMode: false, highWaterMark: 1 << 20 })
    this.#sampleRate = sampleRate
    this.#channels = channels
  }

  /** Required by Readable; data arrives from the native side, not on demand. */
  override _read(): void {}

  get sampleRate(): number {
    return this.#sampleRate
  }

  get channels(): number {
    return this.#channels
  }

  get droppedSamples(): number {
    return this.#droppedSamples
  }

  pushAudio(chunk: Buffer, dropped: number): void {
    if (dropped > 0) {
      this.#droppedSamples += dropped
      this.emit('drop', dropped)
    }
    this.push(chunk)
  }

  finish(): void {
    this.push(null)
  }
}

class CaptureSessionImpl implements CaptureSession {
  #state: CaptureState = 'recording'
  readonly #systemAudio: AudioTrackImpl
  readonly #microphone?: AudioTrackImpl

  constructor(systemAudio: AudioTrackImpl, microphone?: AudioTrackImpl) {
    this.#systemAudio = systemAudio
    this.#microphone = microphone
  }

  get systemAudio(): AudioTrackImpl {
    return this.#systemAudio
  }

  get microphone(): AudioTrackImpl | undefined {
    return this.#microphone
  }

  get state(): CaptureState {
    return this.#state
  }

  pauseRecording(): void {
    if (this.#state === 'stopped') return
    if (this.#state !== 'paused') {
      native.capturePause()
      this.#state = 'paused'
    }
  }

  resumeRecording(): void {
    if (this.#state === 'stopped') return
    if (this.#state !== 'recording') {
      native.captureResume()
      this.#state = 'recording'
    }
  }

  async stopRecording(): Promise<void> {
    if (this.#state === 'stopped') return
    native.captureStop()
    this.#state = 'stopped'
    this.systemAudio.finish()
    this.microphone?.finish()
  }
}

let captureStarting = false

export const capture = {
  async start(
    target: CaptureTarget,
    options: CaptureOptions = {},
  ): Promise<CaptureSession> {
    if (captureStarting || native.isRunning()) {
      throw new AlreadyRunningError('capture already running', -3)
    }
    captureStarting = true
    let session: CaptureSessionImpl | undefined
    const pendingSystemAudio: Array<{ chunk: Buffer; dropped: number }> = []
    const pendingMicrophone: Array<{ chunk: Buffer; dropped: number }> = []

    const receive = (
      track: 'systemAudio' | 'microphone',
      pending: Array<{ chunk: Buffer; dropped: number }>,
      chunk: Buffer,
      info: { dropped: number },
    ) => {
      if (!session) {
        pending.push({ chunk, dropped: info.dropped })
        return
      }
      if (session.state !== 'recording') return
      session[track]?.pushAudio(chunk, info.dropped)
    }

    const onSystemAudio = (chunk: Buffer, info: { dropped: number }) => {
      receive('systemAudio', pendingSystemAudio, chunk, info)
    }
    const onMicrophone = (chunk: Buffer, info: { dropped: number }) => {
      receive('microphone', pendingMicrophone, chunk, info)
    }

    try {
      let nativeTarget: { pid: number } | { systemWide: true }
      if (target.type === 'process') {
        if (!Number.isInteger(target.pid) || target.pid <= 0) {
          throw new TypeError('process pid must be a positive integer')
        }
        nativeTarget = { pid: target.pid }
      } else if (target.type === 'system') {
        nativeTarget = { systemWide: true }
      } else {
        throw new TypeError(`unknown capture target: ${(target as any).type}`)
      }
      if (options.mono !== undefined && typeof options.mono !== 'boolean') {
        throw new TypeError('mono must be a boolean')
      }
      if (options.microphone !== undefined && options.microphone !== 'default') {
        throw new TypeError('microphone must be "default"')
      }
      const formats = await native.captureStart(
        {
          ...nativeTarget,
          mono: options.mono ?? true,
          microphone: options.microphone === 'default',
        },
        onSystemAudio,
        onMicrophone,
      )
      const systemAudio = new AudioTrackImpl(
        formats.systemAudio.sampleRate,
        formats.systemAudio.channels,
      )
      const microphone = formats.microphone
        ? new AudioTrackImpl(formats.microphone.sampleRate, formats.microphone.channels)
        : undefined
      session = new CaptureSessionImpl(systemAudio, microphone)
      for (const { chunk, dropped } of pendingSystemAudio) {
        systemAudio.pushAudio(chunk, dropped)
      }
      for (const { chunk, dropped } of pendingMicrophone) {
        microphone?.pushAudio(chunk, dropped)
      }
      return session
    } catch (error) {
      throw asLibraryError(error)
    } finally {
      captureStarting = false
    }
  },

  get running(): boolean {
    return captureStarting || native.isRunning()
  },
}
