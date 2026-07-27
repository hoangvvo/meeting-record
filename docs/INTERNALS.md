# Internals

Implementation notes and API edge cases. See the README for usage. Tested on macOS 26.5.1, Apple silicon.

### Basic

- **macOS system audio:** CoreAudio process taps (14.2+). Needs `kTCCServiceAudioCapture`.
- **macOS microphone:** `AVAudioEngine` using the current default input device. Needs microphone permission.
- **Windows system audio:** WASAPI process loopback (Win10 build 20348+).
- **Windows microphone:** WASAPI shared-mode capture using the current default input device.
- We don't use virtual audio drivers or ScreenCaptureKit anymore.

On macOS, the permission grant is silent and unbounded. If you need to debug the state, check the TCC database directly (`auth_value` 2 is allowed, 0 is denied):

```sql
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service,client,auth_value from access where service='kTCCServiceAudioCapture'"

```

### Architecture & realtime constraints

The native audio callbacks produce interleaved float32. The Rust and Node bindings copy each track into its own bounded queue before user code runs.

macOS pipeline:

1. `AudioHardwareCreateProcessTap` to mix down specific processes.
2. Wrap it in a private aggregate device. Leave the sub-device list empty so you don't hijack the user's output.
3. Call `AudioDeviceCreateIOProcID` on the aggregate to pull PCM.
4. If requested, run the default microphone through a separate `AVAudioEngine` input tap.

System audio and microphone stay separate. They can negotiate different sample rates; a session pauses, resumes, and stops both together.

Both callbacks carry the first frame's timestamp on the host monotonic clock. The
bindings retain packet boundaries and format metadata while coalescing compatible
packets. Their sample and packet rings are fixed-size; they never queue one runtime
wake per native callback.

Default-device, tap-format, aggregate-device, and AVAudioEngine configuration
changes are monitored. Recovery is serialized, retries three times, and moves the
session through `RECOVERING` to `RUNNING` or terminal `FAILED`. Windows aligns all
per-PID WASAPI clients on QPC and emits one ordered 10 ms mixed track.

The native core intentionally exposes raw synchronized stems. Echo cancellation,
gain control, and noise suppression belong in a downstream processing layer; use
the system track as the AEC reference before mixing when building live-call or
transcription products.

### CoreAudio failure modes

The HAL will return `noErr` and deliver empty streams or permanent hangs.

- **The block API is broken:** `AudioDeviceCreateIOProcIDWithBlock` doesn't work on tap-backed aggregates. The block never fires. Use the function-pointer callback.
- **TCC deadlocks:** If you lack the TCC grant, CoreAudio blocks indefinitely waiting for a GUI prompt. If you call this on a non-GUI main thread, your app deadlocks permanently. Wrap it in a timeout.
- **Zombie audio:** Dead processes keep `kAudioProcessPropertyIsRunningOutput == true` in the HAL. Tapping them yields infinite zeros. Filter by actual process liveness.
- **Bad PIDs:** PIDs are not `AudioObjectID`s. Translate them via `kAudioHardwarePropertyTranslatePIDToProcessObject` or you get `kAudioHardwareBadObjectError`.
- **Format lies:** `kAudioTapPropertyFormat` might say mono 48kHz while the device delivers stereo. Read the format directly from the aggregate's input stream.
- **Non-interleaved stereo:** A tap may deliver two mono buffers. Interleave them
  into preallocated scratch space; forwarding each buffer as if it were a complete
  stereo frame corrupts the stream.
- **Permission timeout lifetime:** Prepare the HAL objects under the timeout, but
  do not arm the host callback or start realtime I/O until preparation succeeds.
  A timed-out worker may finish later and must own no binding pointer.

### Meeting detection

Don't scrape the UI for mute buttons. It requires the Accessibility (AX) grant and breaks whenever the app updates or the user changes their language. We use audio IO heuristics instead.

**Scoring:** Open app (40) + audio output (30) + mic active (25) + URL match (5). Threshold to record is 70.

An open app does nothing. Active audio triggers it. Muted users are recorded if the call itself is active.

**Browser handling:** A YouTube tab looks identical to a Google Meet tab at the process level. Browsers need mic activity or a recognized URL to hit the threshold.

**Process targets:** The UI PID for Chrome, Electron, or Teams is useless for audio capture. You have to find and tap the actual audio helper processes. (Windows handles this natively via `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`).

### Accessibility (AX) hazards

If you use AX to grab titles/URLs, be careful:

- **Sync IPC:** AX calls are synchronous IPC. If Chrome hangs, your app hangs. Enforce a ~250ms timeout.
- **Hidden DOMs:** Chromium/Electron expose nothing until you explicitly set `AXEnhancedUserInterface`.
- **Massive trees:** Browser windows have thousands of elements. Cap your depth and node traversal or you'll burn CPU.

### macOS permission quirks

To get the audio prompt to show up:

1. `NSAudioCaptureUsageDescription` must be in `Info.plist`. The system fails silently without it.
2. Binary must be code-signed.
3. Must be a GUI process (`NSApplication` running).
4. Request the permission **off** the main thread.

There is no public preflight API. We probe by building a throwaway tap and checking if IO starts. We cache this because probing is expensive and disrupts live captures.

Microphone access is a separate TCC grant. A host that requests it must include `NSMicrophoneUsageDescription` and, when sandboxed, the audio-input entitlement.

### Teardown

Do not SIGKILL a process holding a tap. Destroy the tap and aggregate device cleanly first. Crashing mid-capture leaks HAL state and eventually wedges `coreaudiod` system-wide. The user will have to reboot their machine.
