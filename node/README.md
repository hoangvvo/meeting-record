# meeting-record for Node.js

[Full documentation](https://github.com/hoangvvo/meeting-record#readme)

## Install

```sh
npm install meeting-record
```

## Usage

```ts
import * as MeetingRecord from "meeting-record";

// Request system audio permission to record the whole system mix.
await MeetingRecord.permissions.request("system-audio");
// Request microphone permission to record the user voice.
await MeetingRecord.permissions.request("microphone");
// Request accessibility permission to read meeting titles and URLs for detection.
await MeetingRecord.permissions.request("accessibility");

// Watch for meetings starting, changing, and ending.
MeetingRecord.meetings.on("started", (meeting) => {
  console.log("meeting started", meeting);
});
MeetingRecord.meetings.on("updated", (meeting) => {
  console.log("meeting updated", meeting);
});
MeetingRecord.meetings.on("ended", (meeting) => {
  console.log("meeting ended", meeting);
});
MeetingRecord.meetings.watch();

// Or scan for active meetings at any time.
const meeting = (await MeetingRecord.meetings.scan()).find((item) => item.shouldRecord);
if (!meeting) throw new Error("no active meeting");

// Start recording the meeting process and its audio helpers.
// Use `{ type: "system" }` instead to record the whole system mix.
const recording = await MeetingRecord.capture.start(
  { type: "process", pid: meeting.pid },
  { microphone: "default" },
);

// A lost source never stops the recording; its track resumes when it returns.
recording.systemAudio.on("interrupted", (reason, message) => console.warn(reason, message));
recording.systemAudio.on("resumed", () => console.log("system audio resumed"));
recording.microphone?.on("devicechange", (deviceName) => console.log("now on", deviceName));

// Process captured application or system audio.
recording.systemAudio.on("data", (chunk) => {
  console.log(chunk.length, chunk.hostTimeNs, chunk.sampleRate, chunk.channels);
});
recording.systemAudio.on("drop", (samples) => console.warn("dropped", samples));
recording.systemAudio.on("format", (format) => console.log("new format", format));

// Process the microphone track if requested.
recording.microphone?.on("data", (chunk) => {
  console.log(chunk.length, chunk.hostTimeNs, chunk.sampleRate, chunk.channels);
});

await new Promise((resolve) => setTimeout(resolve, 10_000));
await recording.stopRecording();

// Stop watching for meetings when done.
MeetingRecord.meetings.unwatch();
```

## Audio apps

- `meetings.audioApps()`: applications doing audio IO, for a source picker.
- Pass an entry's `pid` to `capture.start()`; no meeting scan or Accessibility grant needed.
- Windows: the target includes future child processes.
- macOS: the target includes current audio helpers; restart capture if they change.

## Mixed output

```ts
const recording = await MeetingRecord.capture.start(
  { type: "system" },
  { microphone: "default", mix: { sampleRate: 24_000 } },
);
recording.mixedAudio.pipe(destination);
recording.mixedAudio.on("level", (rms) => console.log(rms));
recording.mixedAudio.setMicrophoneMuted(true);
```

- `mixedAudio`: mono s16le; raw tracks stay float32.
- Paused time is excluded.
- After stopping, await `end` to consume the final tail.
- No echo cancellation.
- `AbortSignal` in `capture.start()` stops capture; in `permissions.request()` it cancels the wait.
- A lost source pauses only its own track in the mix.

## License

[MIT](./LICENSE.md) © Hoang Vo
