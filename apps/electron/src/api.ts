export type PermissionName = 'system-audio' | 'microphone' | 'accessibility'
export type PermissionStatus = 'granted' | 'denied' | 'unknown' | 'not-required'
export type MeetingEventKind = 'started' | 'updated' | 'ended'

/** `id` is opaque: the backend holds the real meeting object and looks it up
 *  again, so the window never sees a pid. */
export interface MeetingView {
  readonly id: string
  /** Stable identifier (`"zoom"`, `"meet"`, …), not a label. */
  readonly platform: string
  readonly appName: string
  /** Empty without accessibility. */
  readonly title: string
  /** Empty without accessibility. */
  readonly url: string
  readonly isUsingMic: boolean
  readonly isPlayingAudio: boolean
  readonly confidence: number
  readonly shouldRecord: boolean
}

export interface MeetingEvent {
  readonly kind: MeetingEventKind
  readonly meeting: MeetingView
}

/** `path` is the system-audio stem, the only file that exists while recording.
 *  The mix is written on stop. */
export interface RecordingStarted {
  readonly path: string
  readonly sampleRate: number
  readonly channels: number
  readonly microphone: boolean
}

export interface RecordingStopped {
  /** The mixdown of the stems below. */
  readonly path: string
  readonly systemPath: string
  /** null when the mic track was off. */
  readonly micPath: string | null
  readonly seconds: number
  readonly peak: number
  /** Samples lost because the consumer fell behind. */
  readonly dropped: number
}

export interface Meter {
  readonly seconds: number
  readonly level: number
}

export interface Bridge {
  readonly binding: 'node' | 'rust'
  permissionStatus(name: PermissionName): Promise<PermissionStatus>
  /** Resolves when the user answers, or gives up after two minutes. */
  requestPermission(name: PermissionName): Promise<PermissionStatus>
  scan(): Promise<MeetingView[]>
  /** null records every process currently playing audio. Can take ~6s the first time. */
  startRecording(meetingId: string | null, microphone: boolean): Promise<RecordingStarted>
  stopRecording(): Promise<RecordingStopped>
  reveal(path: string): Promise<void>
  onMeetingEvent(handler: (event: MeetingEvent) => void): void
  onMeter(handler: (meter: Meter) => void): void
}
