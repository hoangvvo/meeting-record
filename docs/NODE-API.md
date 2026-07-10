# Node / TypeScript API design

**Status: implemented** in `src/index.ts`. This document records why the API has
the shape it does; the [README](../README.md) is the usage reference.

The aim is that the common case — record calls automatically — is a few lines,
while the individual pieces remain usable on their own.

---

## Design decisions

**Explicit lifecycle events, no state object.** `started` / `updated` / `ended`
rather than a single event carrying `{state: 'recording' | 'idle'}`. A state blob
forces every consumer to diff against what it saw last.

**No progress events.** Capture either runs or does not; there is no measurable
progress to report, and an event that always reports the same thing is noise.

**No upload, no storage.** The library hands the caller bytes. Anything about where
they go belongs in the application.

**An opaque handle, not raw pids.** The JS layer should expose a `Meeting` object
and keep its pid list private. Pids are unstable — apps move audio between helper
processes — so a list captured at detection time can be stale by the time recording
starts. The C API already resolves this inside `mrec_start_meeting()`; the JS layer
should never surface pids at all.

**A boolean, not a threshold.** `meeting.shouldRecord` instead of making callers
compare `confidence >= 70`, which would bake an internal constant into every
consumer.

## Shape

```ts
import mrec from 'meeting-record'
```

Three namespaces, plus one convenience wrapper that covers the common case.

### `mrec.permissions`

```ts
type Permission = 'system-audio' | 'accessibility'
type PermissionStatus = 'granted' | 'denied' | 'unknown' | 'not-required'

mrec.permissions.status(p: Permission): PermissionStatus
mrec.permissions.request(p: Permission): Promise<PermissionStatus>
```

`request()` resolves when the user answers, so callers write the obvious thing:

```ts
if (await mrec.permissions.request('system-audio') !== 'granted') return
```

That is a real improvement over the C API, where you must poll. The polling moves
into the binding.

### `mrec.meetings`

```ts
interface Meeting {
  readonly id: string            // stable across updates; survives pid churn
  readonly platform: 'zoom' | 'teams' | 'meet' | 'webex' | 'slack' | 'discord'
                   | 'browser' | 'unknown'
  readonly appName: string
  readonly title?: string        // undefined without accessibility
  readonly url?: string
  readonly isUsingMic: boolean
  readonly isPlayingAudio: boolean
  readonly confidence: number    // 0-100
  readonly shouldRecord: boolean // confidence >= threshold
  readonly detectedAt: Date
}

mrec.meetings.scan(): Promise<Meeting[]>
mrec.meetings.watch(): void
mrec.meetings.unwatch(): void
mrec.meetings.watching: boolean

mrec.meetings.on('started', (m: Meeting) => void)
mrec.meetings.on('updated', (m: Meeting) => void)
mrec.meetings.on('ended',   (m: Meeting) => void)
```

No pids in the public type: `Meeting` is the handle passed to `capture.start()`.

### `meetingrecord.capture`

Audio is a Node `Readable` stream rather than an event. This is not cosmetic: the
underlying callback runs on a realtime thread and fires whether or not the consumer
keeps up, so backpressure has to be handled somewhere. A stream provides it, and
composes with `pipe()`.

```ts
interface CaptureOptions {
  meeting?: Meeting     // record this meeting's processes
  pids?: number[]       // or specific processes
  systemWide?: boolean  // or everything (silent while the user is muted)
  mono?: boolean        // default true
  format?: 'f32' | 'i16'
}

interface CaptureSession extends Readable {
  readonly sampleRate: number
  readonly channels: number
  stop(): Promise<void>
}

meetingrecord.capture.start(opts: CaptureOptions): Promise<CaptureSession>
meetingrecord.capture.running: boolean
```

```ts
const session = await meetingrecord.capture.start({ meeting, mono: true })
session.pipe(fs.createWriteStream('meeting.f32'))
// or: for await (const chunk of session) transcriber.write(chunk)
await session.stop()
```

`start()` rejects rather than returning an error code — `MREC_ERR_PERMISSION`
becomes a `MrecError` with `.code === 'PERMISSION'`, and the message carries
`mrec_last_error()`.

### `mrec.autoRecord`

The whole point of the library, in one call.

```ts
const controller = mrec.autoRecord({
  onStart: (session, meeting) => {
    session.pipe(fs.createWriteStream(`${meeting.id}.f32`))
  },
  onStop: (meeting) => upload(meeting),
  minConfidence: 70,   // optional
})

controller.stop()
```

Most consumers should need only this. The namespaces above are what it is built
from.

---

## Implementation notes

**Bridging the realtime callback.** The C callback runs on a realtime audio thread
where calling into V8 is forbidden. The binding must:

1. copy frames into a preallocated lock-free ring buffer (realtime-safe), then
2. signal a `Napi::ThreadSafeFunction`, which
3. drains the ring buffer on the Node loop and pushes into the stream.

Never call the TSFN's blocking variants from the audio thread. If the ring buffer
overruns, drop oldest and emit a `'drop'` event with a count — silently losing
audio in a recorder is worse than telling the caller.

**Call `mrec_start_raw`.** `mrec_start` and `mrec_config_defaults` are
`static inline` in the header; only `_raw` is an exported symbol.

**Get `mrec_start` off the main thread.** It can block up to 6 seconds before
the permission grant exists. Use an `AsyncWorker`; this is why `start()` returns a
promise.

**Meeting ids.** The C layer has no ids. Synthesise one in the binding —
`${platform}:${pid}` is enough, keyed to the watcher's own diffing — and hold the
pid list privately so `capture.start({meeting})` can look it up fresh at start
time rather than trusting pids captured at detection time.

**Electron.** A native addon needs prebuilds per Electron ABI, or a rebuild on
every version bump. The alternative is a helper process streaming over stdio, which
avoids ABI coupling entirely and isolates crashes, at the cost of IPC overhead.
Worth choosing deliberately before the addon is written.

---

## Open questions

1. **`EventEmitter` vs `addEventListener`.** EventEmitter is the Node idiom and
   composes with `once()` and async iterators. Recommend EventEmitter with typed
   overloads.
2. **Encoding in or out of scope?** Raw float32 is the primitive, but most
   consumers will want WAV or Opus. Suggest keeping the core raw and shipping
   `meeting-record/encode` separately.
3. **Should `autoRecord` include the microphone?** Both sides of a call are
   usually wanted. Mic capture is not implemented; when it is, `autoRecord`
   should likely mix by default with separate tracks opt-in.
