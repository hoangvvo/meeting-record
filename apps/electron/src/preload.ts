import { contextBridge, ipcRenderer } from "electron";
import type { MeetingEvent, Meter, PermissionName } from "./api";

contextBridge.exposeInMainWorld("MeetingRecord", {
  permissionStatus: (name: PermissionName) => ipcRenderer.invoke("permission:status", name),
  requestPermission: (name: PermissionName) => ipcRenderer.invoke("permission:request", name),
  scan: () => ipcRenderer.invoke("meetings:scan"),
  startRecording: (meetingId: string | null, microphone: boolean) =>
    ipcRenderer.invoke("recording:start", meetingId, microphone),
  stopRecording: () => ipcRenderer.invoke("recording:stop"),
  reveal: (path: string) => ipcRenderer.invoke("file:reveal", path),
  onMeetingEvent: (handler: (event: MeetingEvent) => void) => {
    ipcRenderer.on("meeting:event", (_event, payload: MeetingEvent) => handler(payload));
  },
  onMeter: (handler: (meter: Meter) => void) => {
    ipcRenderer.on("recording:meter", (_event, payload: Meter) => handler(payload));
  },
});
