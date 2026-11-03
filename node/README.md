# meeting-record for Node.js

[![npm](https://img.shields.io/npm/v/meeting-record)](https://www.npmjs.com/package/meeting-record)

System-audio and microphone capture with meeting detection for macOS, Windows, and Linux.
See the [project README](https://github.com/hoangvvo/meeting-record#readme) for requirements, permissions, and supported apps.

```sh
npm install meeting-record
```

## Usage

Capture system audio for ten seconds.

```ts
import { setTimeout as delay } from "node:timers/promises";
import { capture, permissions, Permission } from "meeting-record";

await permissions.request(Permission.SystemAudio);
const recording = await capture.start({ type: "system" });
const stopped = delay(10_000).then(() => recording.stopRecording());

for await (const chunk of recording.systemAudio) {
  console.log(chunk.pcm, chunk.hostTimeNs, chunk.sampleRate, chunk.channels);
}
await stopped;
```

Select an app and include its microphone and a 24 kHz mix.

```ts
import { meetings, MicrophoneSource } from "meeting-record";

await permissions.request(Permission.Microphone);
const [app] = meetings.audioApps();
if (!app) throw new Error("No audio apps");

const appRecording = await capture.start(
  { type: "process", pid: app.pid },
  { microphone: MicrophoneSource.Auto, mix: { sampleRate: 24_000 } },
);
```

Read the mix and pause, resume, or stop the recording.

```ts
appRecording.mixedAudio.on("data", (pcm) => console.log(pcm.byteLength));
appRecording.pauseRecording();
appRecording.resumeRecording();
await delay(10_000);
await appRecording.stopRecording();
```

Scan for meetings or watch for changes.

```ts
const active = (await meetings.scan()).filter((meeting) => meeting.shouldRecord);

meetings.on("started", (meeting) => console.log("Started:", meeting));
meetings.on("updated", (meeting) => console.log("Updated:", meeting));
meetings.on("ended", ({ platform, pid }) => console.log("Ended:", platform, pid));
meetings.watch();

await delay(10_000);
meetings.unwatch();
```

On `ended`, use `platform` and `pid` to remove the meeting. Other payload fields, including `shouldRecord`, may contain the last active observation. Meeting events do not stop capture; call `stopRecording()` and drain the tracks when you want the recording to end.

## License

[MIT](./LICENSE.md) © Hoang Vo
