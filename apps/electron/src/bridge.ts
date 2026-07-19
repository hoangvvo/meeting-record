// the addon can't load in a renderer under contextIsolation, so every library
// call is an IPC round trip to main
import type {
  Bridge,
  MeetingEvent,
  MeetingView,
  Meter,
  PermissionName,
  PermissionStatus,
  RecordingStarted,
  RecordingStopped,
} from './api'

declare global {
  interface Window {
    /** Injected by src/preload.ts. */
    readonly MeetingRecord: {
      permissionStatus(name: PermissionName): Promise<PermissionStatus>
      requestPermission(name: PermissionName): Promise<PermissionStatus>
      scan(): Promise<MeetingView[]>
      startRecording(meetingId: string | null, microphone: boolean): Promise<RecordingStarted>
      stopRecording(): Promise<RecordingStopped>
      reveal(path: string): Promise<void>
      onMeetingEvent(handler: (event: MeetingEvent) => void): void
      onMeter(handler: (meter: Meter) => void): void
    }
  }
}

export const bridge: Bridge = {
  binding: 'node',
  permissionStatus: (name) => window.MeetingRecord.permissionStatus(name),
  requestPermission: (name) => window.MeetingRecord.requestPermission(name),
  scan: () => window.MeetingRecord.scan(),
  startRecording: (meetingId, microphone) =>
    window.MeetingRecord.startRecording(meetingId, microphone),
  stopRecording: () => window.MeetingRecord.stopRecording(),
  reveal: (path) => window.MeetingRecord.reveal(path),
  onMeetingEvent: (handler) => window.MeetingRecord.onMeetingEvent(handler),
  onMeter: (handler) => window.MeetingRecord.onMeter(handler),
}
