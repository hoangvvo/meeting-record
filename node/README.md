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
const meeting = MeetingRecord.meetings.scan().find((item) => item.shouldRecord);
if (!meeting) throw new Error("no active meeting");

// Start recording the meeting process and its audio helpers.
// Use `{ type: "system" }` instead to record the whole system mix.
const recording = await MeetingRecord.capture.start(
  { type: "process", pid: meeting.pid },
  { microphone: "default" },
);

recording.on("interrupted", () => console.warn("audio device changed; recovering"));
recording.on("recovered", () => console.log("capture recovered"));
recording.on("failed", (error) => console.error("capture failed", error));

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

## License

[MIT](./LICENSE.md) © Hoang Vo
