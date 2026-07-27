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

const recording = await meetingRecord.capture.start({ type: "process", pid: meeting.pid });
recording.systemAudio.on("data", (samples) => console.log(samples.length));
await new Promise((resolve) => setTimeout(resolve, 10_000));
await recording.stopRecording();
meetingRecord.meetings.unwatch();
```

## License

[MIT](./LICENSE.md) © Hoang Vo
