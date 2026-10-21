# meeting-record for Node.js

System-audio and microphone capture with meeting detection for macOS and Windows.
See the [platform requirements](https://github.com/hoangvvo/meeting-record#requirements).

```sh
npm install meeting-record
```

## Usage

```ts
import { capture, permissions } from "meeting-record";

await permissions.request("system-audio");
const recording = await capture.start({ type: "system" });

// Raw float32 PCM with timing and format metadata.
recording.systemAudio.on("data", (chunk) => {
  console.log(chunk.pcm, chunk.hostTimeNs, chunk.sampleRate, chunk.channels);
});

await new Promise((resolve) => setTimeout(resolve, 10_000));
// Stop capture; buffered audio remains readable until the streams end.
await recording.stopRecording();
```

- **App picker:** `meetings.audioApps()` lists available apps.
- **Process capture:** `{ type: "process", pid }` captures the app and its audio helpers.
- **Microphone:** optional; request permission before including it.

```ts
await permissions.request("microphone");
const recording = await capture.start(
  { type: "process", pid }, // PID from the app picker or a detected meeting.
  {
    mono: false, // Stereo system audio; defaults to mono. The mic stays mono.
    microphone: "default", // Follow the OS input device.
  },
);
```

Use `microphone: "auto"` to prefer the captured app's mic on Windows, then the
communications default. On macOS, both modes use the default input.

## Meetings

```ts
import { meetings } from "meeting-record";

const active = (await meetings.scan()).filter((meeting) => meeting.shouldRecord);
meetings.on("started", (meeting) => console.log(meeting));
meetings.watch();

// Later: meetings.unwatch();
```

- The watcher also emits `updated` and `ended`.
- Request `"accessibility"` permission on macOS to include titles and URLs.
- Enumeration failures throw library errors.

## Audio output

Raw tracks are object-mode streams of float32 packets. Read bytes from `chunk.pcm`
and use each packet's timestamp, sample rate, and channel count.

```ts
const recording = await capture.start(
  { type: "system" },
  { microphone: "default", mix: { sampleRate: 24_000 } }, // Combined mono s16le at 24 kHz.
);
const mixed = recording.mixedAudio!;
mixed.pipe(destination);
// Mute the mic in the mix without changing the raw microphone track.
mixed.setMicrophoneMuted(true);
```

- **Pause/resume:** `pauseRecording()` and `resumeRecording()` control capture.
- **Stop:** `stopRecording()` is asynchronous and idempotent. Drain streams to `end` for the final audio.
- **Cancellation:** pass an `AbortSignal` in the capture options.
- **Source loss:** the session stays alive; tracks emit `degraded`, `interrupted`, and `resumed`.
- **Device changes:** the microphone emits `devicechange`.
- **App exit:** a relaunched app requires a new capture session.

## License

[MIT](./LICENSE.md) © Hoang Vo
