import { app, BrowserWindow, ipcMain, shell } from "electron";
import path from "node:path";
import * as MeetingRecord from "meeting-record";
import type {
  MeetingEventKind,
  MeetingView,
  PermissionName,
  RecordingStarted,
  RecordingStopped,
} from "./api";
import { WavWriter } from "./wav";
import { mix } from "./mix";

const METER_INTERVAL_MS = 100;

interface Track {
  writer: WavWriter;
  peak: number;
  dropped: number;
}

interface Recording {
  session: MeetingRecord.CaptureSession;
  system: Track;
  microphone: Track | null;
  mixedPath: string;
  startedAt: number;
  meter: NodeJS.Timeout;
  /** peak across both tracks since the last meter tick */
  windowPeak: number;
}

// Capture and watching are shared across windows.
const detected = new Map<string, number>();
let recording: Recording | null = null;
let win: BrowserWindow | null = null;

const idOf = (meeting: MeetingRecord.Meeting): string => `${meeting.platform}:${meeting.pid}`;

function toView(meeting: MeetingRecord.Meeting): MeetingView {
  return {
    id: idOf(meeting),
    platform: meeting.platform,
    appName: meeting.appName,
    title: meeting.title,
    url: meeting.url,
    isUsingMic: meeting.isUsingMic,
    isPlayingAudio: meeting.isPlayingAudio,
    confidence: meeting.confidence,
    shouldRecord: meeting.shouldRecord,
  };
}

function send(channel: string, payload: unknown): void {
  if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

function onMeeting(kind: MeetingEventKind, meeting: MeetingRecord.Meeting): void {
  if (kind === "ended") detected.delete(idOf(meeting));
  else detected.set(idOf(meeting), meeting.pid);
  send("meeting:event", { kind, meeting: toView(meeting) });
}

function watchMeetings(): void {
  MeetingRecord.meetings.on("started", (meeting) => onMeeting("started", meeting));
  MeetingRecord.meetings.on("updated", (meeting) => onMeeting("updated", meeting));
  MeetingRecord.meetings.on("ended", (meeting) => onMeeting("ended", meeting));
  MeetingRecord.meetings.watch();
}

/** `[mixed, system stem, mic stem]` for one recording. */
function wavPaths(): [string, string, string] {
  const name = `meeting-record-electron-${Date.now()}`;
  const inDownloads = (suffix: string) =>
    path.join(app.getPath("downloads"), `${name}${suffix}.wav`);
  return [inDownloads(""), inDownloads("-system"), inDownloads("-mic")];
}

// Prefer a process target when an audio app is available.
function playingPid(): number {
  const playing = MeetingRecord.meetings.audioProcesses().find((entry) => entry.isPlayingAudio);
  if (!playing) throw new Error("nothing is playing audio");
  return playing.pid;
}

/** Drains one track into its own WAV. */
function drain(
  source: MeetingRecord.AudioTrack,
  writer: WavWriter,
  into: () => Recording | null,
): Track {
  const track: Track = { writer, peak: 0, dropped: 0 };

  // The addon delivers packets on the main thread.
  source.on("data", (chunk: MeetingRecord.AudioChunk) => {
    const peak = writer.write(chunk.pcm);
    if (peak > track.peak) track.peak = peak;
    const state = into();
    if (state && peak > state.windowPeak) state.windowPeak = peak;
  });
  source.on("drop", (count: number) => {
    track.dropped += count;
  });

  return track;
}

async function startRecording(
  targetId: string | null,
  microphone: boolean,
): Promise<RecordingStarted> {
  if (recording) throw new Error("already recording");

  const pid = targetId === null ? playingPid() : detected.get(targetId);
  if (pid === undefined) throw new Error("meeting is no longer detected");

  const session = await MeetingRecord.capture.start(
    { type: "process", pid },
    microphone ? { microphone: "auto" } : {},
  );

  const [mixedPath, systemPath, micPath] = wavPaths();
  let system: WavWriter;
  let mic: WavWriter | null = null;
  try {
    system = await WavWriter.create(
      systemPath,
      session.systemAudio.sampleRate,
      session.systemAudio.channels,
    );
    if (session.microphone) {
      mic = await WavWriter.create(
        micPath,
        session.microphone.sampleRate,
        session.microphone.channels,
      );
    }
  } catch (error) {
    // don't leave the tap running just because we couldn't open a file
    await session.stopRecording();
    throw error;
  }

  const meter = setInterval(() => {
    if (!recording) return;
    const level = recording.windowPeak;
    recording.windowPeak = 0;
    send("recording:meter", { seconds: (Date.now() - recording.startedAt) / 1000, level });
  }, METER_INTERVAL_MS);

  const current = () => recording;
  const state: Recording = {
    session,
    system: drain(session.systemAudio, system, current),
    microphone: session.microphone && mic ? drain(session.microphone, mic, current) : null,
    mixedPath,
    startedAt: Date.now(),
    meter,
    windowPeak: 0,
  };

  recording = state;
  return {
    path: system.path,
    sampleRate: session.systemAudio.sampleRate,
    channels: session.systemAudio.channels,
    microphone: state.microphone !== null,
  };
}

async function stopRecording(): Promise<RecordingStopped> {
  const state = recording;
  if (!state) throw new Error("not recording");

  recording = null;
  clearInterval(state.meter);
  await state.session.stopRecording();
  // Finish both stems before mixing them.
  await state.system.writer.finish();
  await state.microphone?.writer.finish();

  const systemPath = state.system.writer.path;
  const micPath = state.microphone?.writer.path ?? null;
  const tracks = [state.system, state.microphone].filter((track) => track !== null);
  const seconds = Math.max(...tracks.map((track) => track.writer.seconds));
  const dropped = tracks.reduce((total, track) => total + track.dropped, 0);

  let mixedPath = state.mixedPath;
  let peak: number;
  try {
    peak = await mix(systemPath, micPath, mixedPath);
  } catch (error) {
    // the stems survive a failed mix, so hand back the one that is definitely playable
    console.error("could not mix the stems:", error);
    mixedPath = systemPath;
    peak = Math.max(...tracks.map((track) => track.peak));
  }

  return { path: mixedPath, systemPath, micPath, seconds, peak, dropped };
}

ipcMain.handle("permission:status", (_event, name: PermissionName) =>
  MeetingRecord.permissions.status(name),
);
ipcMain.handle("permission:request", (_event, name: PermissionName) =>
  MeetingRecord.permissions.request(name),
);

ipcMain.handle("meetings:scan", async () => {
  const found = await MeetingRecord.meetings.scan();
  detected.clear();
  for (const meeting of found) detected.set(idOf(meeting), meeting.pid);
  return found.map(toView);
});

ipcMain.handle("recording:start", (_event, targetId: string | null, microphone: boolean) =>
  startRecording(targetId, microphone),
);
ipcMain.handle("recording:stop", () => stopRecording());
ipcMain.handle("file:reveal", (_event, target: string) => shell.showItemInFolder(target));

function createWindow(): void {
  win = new BrowserWindow({
    width: 720,
    height: 820,
    backgroundColor: "#ffffff",
    // preload stays CJS: an ESM preload needs sandbox off
    webPreferences: { preload: path.join(import.meta.dirname, "preload.js") },
  });

  if (MAIN_WINDOW_VITE_DEV_SERVER_URL) {
    void win.loadURL(MAIN_WINDOW_VITE_DEV_SERVER_URL);
  } else {
    void win.loadFile(
      path.join(import.meta.dirname, `../renderer/${MAIN_WINDOW_VITE_NAME}/index.html`),
    );
  }
}

void app.whenReady().then(() => {
  createWindow();
  watchMeetings();
});

app.on("window-all-closed", () => app.quit());

let quitting = false;
app.on("before-quit", (event) => {
  if (!recording || quitting) return;
  // Finish native teardown before quitting.
  event.preventDefault();
  quitting = true;
  void stopRecording()
    .catch(() => {})
    .finally(() => app.quit());
});

app.on("will-quit", () => MeetingRecord.meetings.unwatch());

// ctrl-c under `electron-forge start`
for (const signal of ["SIGINT", "SIGTERM"] as const) process.on(signal, () => app.quit());
