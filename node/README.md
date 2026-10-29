# meeting-record for Node.js

System-audio and microphone capture with meeting detection for macOS, Windows, and Linux.

## Install

```sh
npm install meeting-record
```

- Requires Node.js 22.13+ (22.x) or 24+.
- See the [platform requirements and permissions](https://github.com/hoangvvo/meeting-record#requirements) for app configuration.

## Capture

```ts
import { setTimeout as delay } from "node:timers/promises";
import { capture, permissions, Permission } from "meeting-record";

await permissions.request(Permission.SystemAudio);
const recording = await capture.start({ type: "system" });

// Stop after ten seconds; keep reading until the stream ends.
const stopped = delay(10_000).then(() => recording.stopRecording());
for await (const chunk of recording.systemAudio) {
  // Raw float32 PCM, with the format and capture time of this packet.
  console.log(chunk.pcm, chunk.hostTimeNs, chunk.sampleRate, chunk.channels);
}
await stopped;
```

| Target                        | Value passed to `capture.start()` |
| ----------------------------- | --------------------------------- |
| System audio                  | `{ type: "system" }`              |
| One app and its audio helpers | `{ type: "process", pid }`        |

- **App picker:** `meetings.audioApps()` lists available apps. Use the selected entry's `pid`; no meeting scan is required.
- **Meeting capture:** a detected meeting's `pid` is also a process target.
- **Permissions:** `permissions.request()` returns a `PermissionStatus`; `permissions.status()` checks the current status without prompting.
- **Sessions:** one recording at a time per process.

### Add a microphone

Request microphone permission, then pass `microphone` in the capture options:

```ts
import { MicrophoneSource } from "meeting-record";

await permissions.request(Permission.Microphone);
const recording = await capture.start(
  { type: "system" },
  {
    mono: false, // Stereo system audio; defaults to mono. The mic stays mono.
    microphone: MicrophoneSource.Default,
  },
);
```

- `MicrophoneSource.Default` follows the OS input device.
- `MicrophoneSource.Auto` prefers the captured app's mic on Windows/Linux, then the default (communications default on Windows). On macOS, it uses the default input.
- Without `microphone`, only system audio is captured.

## Audio output

| Output       | Access                  | Data                                                                         |
| ------------ | ----------------------- | ---------------------------------------------------------------------------- |
| System audio | `recording.systemAudio` | Object-mode stream of `AudioChunk` packets                                   |
| Microphone   | `recording.microphone`  | Same packet type; present when requested                                     |
| Mixed audio  | `recording.mixedAudio`  | Byte stream of mono signed 16-bit little-endian PCM; present when configured |

- **Raw samples:** `chunk.pcm` contains interleaved float32 PCM.
- **Packet metadata:** `hostTimeNs`, `frameCount`, `sampleRate`, `channels`, and `droppedSamples`.
- **Reading:** use `data` listeners or `for await...of` on each raw track. Use each packet's format; it can change during capture.
- **Slow readers:** raw tracks report discarded samples through `drop` events and `droppedSamples`.
- See [capture formats and channels](https://github.com/hoangvvo/meeting-record#capture) for sample rates and platform behavior.

### Mixed audio

After requesting the audio and microphone permissions, start with `mix` to add a combined stream:

```ts
const recording = await capture.start(
  { type: "system" },
  {
    microphone: MicrophoneSource.Default,
    mix: { sampleRate: 24_000 }, // Combined mono s16le at 24 kHz.
  },
);

recording.mixedAudio.on("data", (pcm) => console.log(pcm.byteLength));
recording.mixedAudio.setMicrophoneMuted(true); // Affects the mix only.
```

- Mixed audio is a standard byte stream: it can also be piped to a writable.
- Raw tracks remain available alongside the mix.

## Controls and events

| Action                    | API                                                            |
| ------------------------- | -------------------------------------------------------------- |
| Pause / resume            | `recording.pauseRecording()` / `recording.resumeRecording()`   |
| Stop                      | `await recording.stopRecording()`; safe to call more than once |
| Check recording state     | `recording.state`                                              |
| Check source availability | `track.availability`                                           |
| Cancel start or recording | Pass an `AbortSignal` as `CaptureOptions.signal`               |

- **Pause:** affects the whole recording; paused time is omitted.
- **Stop:** keep reading each output until `end` to drain queued audio.
- **Source loss:** the session stays alive. Track availability is `live`, `degraded`, or `interrupted`.
- **App exit:** system audio stops when all captured apps exit; the microphone and session stay alive. Start a new session for a relaunched app.

Attach source-event listeners immediately after awaiting `capture.start()`:

```ts
recording.systemAudio.on("interrupted", (reason, message) => console.log(reason, message));
recording.systemAudio.on("degraded", (reason, message) => console.log(reason, message));
recording.systemAudio.on("resumed", () => console.log("System audio resumed"));
recording.microphone?.on("devicechange", (name) => console.log("Microphone:", name));
```

## Meetings

```ts
import { meetings } from "meeting-record";

const active = (await meetings.scan()).filter((meeting) => meeting.shouldRecord);

meetings.on("started", (meeting) => console.log("Started:", meeting));
meetings.on("updated", (meeting) => console.log("Updated:", meeting));
meetings.on("ended", (meeting) => console.log("Ended:", meeting));
meetings.watch();

// Later: meetings.unwatch();
```

- Use `shouldRecord` to decide whether a meeting is ready to record.
- The library incorporates call controls and audio signals into `confidence` and `shouldRecord`; no consumer-side call-state logic is required.
- Request `Permission.Accessibility` on macOS for call-state checks, browser detection, titles, and URLs; relaunch after granting it.
- See [supported services and detection behavior](https://github.com/hoangvvo/meeting-record#detection).

## Types and errors

- **Constants:** `Platform`, `Permission`, `PermissionStatus`, `MicrophoneSource`, `CaptureState`, `TrackAvailability`, and `TrackInterruptionReason` have matching union types.
- **Events:** names and callback arguments are checked by TypeScript, including chained `on`, `once`, and `off` calls.
- **Library failures:** capture, permissions, and enumeration use `MeetingRecordError` subclasses, with a numeric `code`.
- **Invalid arguments:** throw `TypeError`.
- **Cancellation:** aborting rejects pending work with the signal's reason and stops an active recording.

## License

[MIT](./LICENSE.md) © Hoang Vo
