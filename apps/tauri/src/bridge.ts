import { invoke } from '@tauri-apps/api/core'
import { listen } from '@tauri-apps/api/event'
import type {
  Bridge,
  MeetingEvent,
  MeetingView,
  Meter,
  PermissionStatus,
  RecordingStarted,
  RecordingStopped,
} from './api'

export const bridge: Bridge = {
  binding: 'rust',
  permissionStatus: (name) => invoke<PermissionStatus>('permission_status', { name }),
  requestPermission: (name) => invoke<PermissionStatus>('request_permission', { name }),
  scan: () => invoke<MeetingView[]>('scan'),
  startRecording: (meetingId, microphone) =>
    invoke<RecordingStarted>('start_recording', { targetId: meetingId, microphone }),
  stopRecording: () => invoke<RecordingStopped>('stop_recording'),
  reveal: (path) => invoke('reveal', { path }),
  onMeetingEvent: (handler) => {
    void listen<MeetingEvent>('meeting:event', (event) => handler(event.payload))
  },
  onMeter: (handler) => {
    void listen<Meter>('recording:meter', (event) => handler(event.payload))
  },
}
