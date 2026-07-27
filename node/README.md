# meeting-record for Node.js

[Full documentation](https://github.com/hoangvvo/meeting-record#readme)

## Install

```sh
npm install meeting-record
```

## Usage

```ts
import * as meetingRecord from "meeting-record";

meetingRecord.meetings.on("started", console.log);
meetingRecord.meetings.on("ended", console.log);
meetingRecord.meetings.watch();

await meetingRecord.permissions.request("system-audio");

const meeting = meetingRecord.meetings.scan().find((item) => item.shouldRecord);
if (!meeting) throw new Error("no active meeting");

const recording = await meetingRecord.capture.start(
  { type: "process", pid: meeting.pid },
  { microphone: "default" },
);

recording.on("interrupted", () => console.warn("audio device changed; recovering"));
recording.on("recovered", () => console.log("capture recovered"));
recording.on("failed", (error) => console.error("capture failed", error));

recording.systemAudio.on("data", (chunk) => {
  console.log(chunk.length, chunk.hostTimeNs, chunk.sampleRate, chunk.channels);
});
recording.systemAudio.on("drop", (samples) => console.warn("dropped", samples));
recording.systemAudio.on("format", (format) => console.log("new format", format));
await new Promise((resolve) => setTimeout(resolve, 10_000));
await recording.stopRecording();
meetingRecord.meetings.unwatch();
```

## License

[MIT](./LICENSE.md) © Hoang Vo
