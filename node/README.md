# meeting-record for Node.js

System-audio and microphone capture with meeting detection for macOS and Windows.
See the [platform requirements](https://github.com/hoangvvo/meeting-record#requirements).

```sh
npm install meeting-record
```

## Usage

```ts
import { capture, permissions, Permission } from "meeting-record";

await permissions.request(Permission.SystemAudio);
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
import { MicrophoneSource } from "meeting-record";

await permissions.request(Permission.Microphone);
const recording = await capture.start(
  { type: "process", pid }, // PID from the app picker or a detected meeting.
  {
    mono: false, // Stereo system audio; defaults to mono. The mic stays mono.
    microphone: MicrophoneSource.Default, // Follow the OS input device.
  },
);
```

Use `MicrophoneSource.Auto` to prefer the captured app's mic on Windows, then the
communications default. On macOS, both modes use the default input.

## Types and constants

Domain values have named constants and matching union types: `Platform`,
`Permission`, `PermissionStatus`, `MicrophoneSource`, `CaptureState`,
`TrackAvailability`, and `TrackInterruptionReason`.

```ts
import { Platform, PermissionStatus } from "meeting-record";

const platform: Platform = Platform.Zoom;
if (permissions.status(Permission.SystemAudio) === PermissionStatus.Granted) {
  // Ready to capture system audio.
}
```

Event names use string literals, such as `meetings.on("started", handler)` and
`recording.systemAudio.once("interrupted", handler)`. Event names and callback
arguments are checked across `on`, `once`, `off`, and the other listener methods,
including chained calls. Capture targets use a discriminated union:
`{ type: "system" }` or `{ type: "process", pid }`.

## Meetings

```ts
import { meetings, Platform } from "meeting-record";

const active = (await meetings.scan()).filter((meeting) => meeting.shouldRecord);
meetings.on("started", (meeting) => {
  if (meeting.platform === Platform.Zoom) console.log(meeting);
});
meetings.watch();

// Later: meetings.unwatch();
```

- The watcher also emits `updated` and `ended`.
- Request `Permission.Accessibility` on macOS to include titles and URLs.
- Enumeration failures throw library errors.

## Audio output

Raw tracks are object-mode streams of float32 packets. Read bytes from `chunk.pcm`
and use each packet's timestamp, sample rate, and channel count.

```ts
const recording = await capture.start(
  { type: "system" },
  { microphone: MicrophoneSource.Default, mix: { sampleRate: 24_000 } }, // Combined mono s16le at 24 kHz.
);
const mixed = recording.mixedAudio;
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
