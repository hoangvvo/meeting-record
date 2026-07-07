# Internals

Implementation notes for the platform APIs this library uses, and the failure modes
they exhibit. For usage, see the [README](../README.md).

Verified on macOS 26.5.1, Apple silicon.

---

## Mechanism

| | macOS | Windows |
|---|---|---|
| Audio capture | CoreAudio process taps (`CATapDescription` + `AudioHardwareCreateProcessTap`) | WASAPI process loopback (`ActivateAudioInterfaceAsync`) |
| Minimum OS | macOS 14.2 | Windows 10 build 20348 |
| Permission | `kTCCServiceAudioCapture`, one-time | none |
| Recording indicator | none | none |
| Detection | Accessibility API (`AXObserver`, `AXWebArea`) | audio sessions + `EnumWindows` |

Neither platform requires a virtual audio device or screen capture. On macOS the
earlier approach was ScreenCaptureKit, which needs the screen-recording permission
and shows an indicator; process taps need neither.

### Permission state

The macOS grant is stored in TCC under `kTCCServiceAudioCapture`, separate from
both microphone and screen recording. It is requested once and persists, and the
system shows nothing while a tap is active — no menu-bar indicator and no entry in
Control Center. Once granted, capture is silent and unbounded.

The current state for a bundle id can be read directly:

```sql
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service,client,auth_value from access
   where service='kTCCServiceAudioCapture'"
```

`auth_value` 2 is allowed, 0 is denied.

---

## Architecture

```
native/include/meeting-record.h        one C ABI for both platforms
native/macos/                  Swift: CoreAudio process taps
  AudioProcesses.swift           HAL property helpers + audio process registry
  TapCapture.swift               tap -> aggregate device -> IOProc
  Permissions.swift              kTCCServiceAudioCapture probe / request
  CaptureState.swift             realtime-safe callback plumbing
  Timeout.swift                  never let CoreAudio hang the host
  Exports.swift                  @_cdecl C ABI
  MeetingCatalog.swift           which apps/URLs count as meetings
  Accessibility.swift            AX titles + browser URLs (optional enrichment)
  MeetingDetector.swift          fuses the detection signals
  MeetingWatcher.swift           event-driven start/update/end
  MeetingExports.swift           @_cdecl C ABI
native/windows/
  ProcessLoopback.cpp            WASAPI process loopback
  MeetingDetector.cpp            audio sessions + EnumWindows
examples/
  selftest.c                     audio: end-to-end verification
  selftest_main.m                GUI host (required for the TCC prompt)
  meetingtest.c                  detection: scan + live watching
tests/
  layout_check.c                 guards hand-computed struct offsets
  catalog_test.swift             classification logic
scripts/build-macos.sh
```

The callback fires on a realtime audio thread and hands you interleaved float32.
Do not allocate, lock, or enter a managed runtime there — push into a ring buffer
and drain elsewhere.

### macOS pipeline

1. `AudioHardwareCreateProcessTap` with a `CATapDescription` — a mixdown of
   specific processes, or a global mixdown.
2. A **private aggregate device** listing that tap. The default output device is
   the aggregate's *main sub-device* purely as a clock source; the sub-device list
   stays empty so the user's output path is never touched.
3. `AudioDeviceCreateIOProcID` on the aggregate to pull PCM.

---

## Failure modes

All four are silent: the API reports success and produces a stream that looks
correct but is empty or never delivered.

**1. `AudioDeviceCreateIOProcIDWithBlock` does not work on tap-backed
aggregates.** The block variant returns `noErr`, the device reports
`IsRunning == 0`, and your block is *never invoked*. Use the C-function-pointer
`AudioDeviceCreateIOProcID`. Switching between the two variants, with no other
change, takes the same code from 0 frames to 231,424 frames in 6 seconds.

**2. Before the TCC grant exists, CoreAudio blocks forever instead of failing.**
`AudioDeviceCreateIOProcID` sits in a mach round-trip inside
`HALC_ProxyIOContext::_TellServerAboutStreamUsage`: coreaudiod is waiting on
tccd, which is waiting for a prompt. The prompt requires a running
`NSApplication` and cannot be drawn by the thread that is blocked, so calling
this on the main thread of a non-GUI process deadlocks permanently. `Timeout.swift`
exists solely to contain this.

**3. The HAL reports dead processes as actively playing.** Process objects
outlive their processes and keep `kAudioProcessPropertyIsRunningOutput == true`.
A tap built over them creates a running aggregate with the correct sample rate
and frame count that delivers nothing but zeros. Always filter for liveness —
`AudioProcessRegistry.isAlive`.

**4. A global tap is post-mute; a process tap is not.** Tap the whole system mix
and you record digital silence whenever the user mutes their speakers — fatal for
a meeting recorder. Tapping specific processes reads each app's stream earlier in
the graph. This library therefore defaults to per-process capture.

Two smaller ones:

- **PIDs are not `AudioObjectID`s.** Passing a raw pid where the tap API wants a
  process object fails with `kAudioHardwareBadObjectError` (`'!obj'`). Translate
  via `kAudioHardwarePropertyTranslatePIDToProcessObject`.
- **Read the format from the aggregate's input stream**, not
  `kAudioTapPropertyFormat`. They disagree: the tap reported mono 48kHz while the
  device delivered stereo.

---

## Meeting detection

How detection decides a call is live. For the API, see the
[README](../README.md#detecting-meetings).

### Four layered signals

| layer | signal | permission | can it be wrong? |
|---|---|---|---|
| 1 | known conferencing app running | none | no |
| 2 | that app is doing audio IO | none | no |
| 3 | window title / tab URL | Accessibility | yes — empty without the grant |
| 4 | participants, mute state | Accessibility | yes — depends on UI language |

Layers 1–2 are exact, free, language-independent, and sufficient to decide
"record now". Recording is never gated on layers 3–4.

The alternative — reading call state out of the UI, by matching control labels such
as a mute button — requires the Accessibility grant and a translation table for
every language the target app ships, per app and per browser. Audio-IO state
answers the same question without either dependency.

### Confidence

`known app 40 + audio output 30 + mic 25 + recognised URL 5`, threshold **70**.

So an app merely being *open* (40) never triggers recording; a live call does. The
weights make "someone is talking and I can hear them" (70) enough on its own, so a
muted user is still recorded.

### Why a browser needs more evidence

At the process level a YouTube tab and a Google Meet tab are identical — same
executable, same helper processes, same audio session. So a browser only counts
as a meeting when either the URL/title names a known service, **or** the
microphone is live (a page only gets the mic with explicit user consent).

### Which pid to record is not the pid you see

Chrome, Electron and Teams render audio from helper processes. `mrec_meeting.pid`
is the process the *user* thinks of as the app; `audio_pids` is what you actually
have to tap. On Windows the same problem is solved by
`PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`.

### Accessibility hazards

Three constraints apply to any AX traversal:

- **AX calls are synchronous IPC into the target app.** A hung app hangs *you*.
  Always set a messaging timeout; this library uses 250ms.
- **Chromium and Electron expose no tree until asked.** Set
  `AXEnhancedUserInterface` on the application element or `AXWebArea` will not
  exist.
- **Trees are huge.** A Chrome window is thousands of elements. Bound traversal by
  depth *and* node count, and never descend into a web area's DOM.

---

## Permission model (macOS)

Two independent grants. The audio one is required; the Accessibility one only buys
titles and URLs.

`kTCCServiceAudioCapture` is separate from microphone *and* from screen
recording, though System Settings files it under **Privacy & Security → Screen &
System Audio Recording**.

Your app must:

1. ship `NSAudioCaptureUsageDescription` in `Info.plist` — without it the system
   refuses to prompt at all;
2. be code-signed with a stable identity;
3. be a GUI process with a running `NSApplication` (Electron and native apps
   already are);
4. call `mrec_request_audio_permission()` **off** the main thread.

There is no public preflight API, so `mrec_audio_permission_status()` probes by
building a throwaway tap and checking whether IO starts. It is time-bounded and
caches a granted result, because probing is not free and doing it next to a live
capture can disturb it.

---

### Do not SIGKILL a process holding a tap

Destroy the tap and aggregate device first. Killing mid-capture leaks HAL state
and eventually wedges `coreaudiod` for the whole machine. Every test binary here
carries a watchdog for this reason.

---

Verification status and troubleshooting live in the
[README](../README.md#status).
