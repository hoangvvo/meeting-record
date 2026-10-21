# Internals

Tested on macOS 26.5.1 (Apple silicon) and Windows 11 build 26200 (x86-64).

## Backends

| Platform | System audio                           | Microphone                                              |
| -------- | -------------------------------------- | ------------------------------------------------------- |
| macOS    | CoreAudio process tap (14.2+)          | `AVAudioEngine`, default input                          |
| Windows  | WASAPI process loopback (build 20348+) | WASAPI shared mode, default input or automatic call mic |

- No virtual audio drivers. No ScreenCaptureKit.
- macOS TCC state (`auth_value` 2 = allowed, 0 = denied):

```sql
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service,client,auth_value from access where service='kTCCServiceAudioCapture'"
```

## Pipeline

- Native callbacks: interleaved float32, first-frame timestamp on the host monotonic clock.
- Bindings: one fixed-size queue per track; coalesce compatible packets; no wake per callback.
- System and microphone: separate tracks and formats; pause, resume and stop together. Stop ends production before the bindings close their queues; consumers retain the captured tail.
- Node raw tracks use object-mode packets with a float32 `pcm` Buffer. Mixed output stays a byte stream.
- Binding mixers omit long all-source gaps (> 1 s); a missing single source contributes silence while others continue.
- macOS: process tap → private aggregate device (empty sub-device list) → `AudioDeviceCreateIOProcID`.
- macOS microphone: separate `AVAudioEngine` input tap.
- Windows: one loopback client per PID, aligned on QPC into one 10 ms track.
- Source loss, shared by both backends: `native/common/tracks.c`. Recording never stops for it.
- Lost source: reopen at once (published as a device change), else `INTERRUPTED` and retry every 500 ms until it returns, then `RESUMED`. A device notification retries at once (Windows `IMMNotificationClient`, macOS default-input and default-output listeners); stop ends pending retries at once.
- Microphone: `DEVICE_CHANGED` on a new device. Events are serialized and never arrive after stop.
- Every captured app exited → system `INTERRUPTED` (`APP_EXITED`); stop system callbacks and retries, and keep the microphone/session alive. Do not follow a relaunched app.
- Track recovery publications carry a session identity. A stale retry cannot change a replacement session.
- Partial source loss is `DEGRADED`; `INTERRUPTED` means every source is unavailable.
- Windows mixer: a stall of every source over 1 s is skipped, not filled; timestamps show the gap.
- No echo cancellation, gain control or noise suppression. Use the system track as the AEC reference downstream.

## CoreAudio rules

- Use `AudioDeviceCreateIOProcID`; the block variant never fires on tap aggregates.
- Create taps under a timeout; without a TCC grant the call blocks forever.
- Filter targets by process liveness.
- Translate PIDs with `kAudioHardwarePropertyTranslatePIDToProcessObject`.
- Read the format from the aggregate's input stream, not `kAudioTapPropertyFormat`.
- Interleave non-interleaved stereo into preallocated scratch.
- Arm callbacks and start IO only after preparation succeeds.

## WASAPI rules

- Pin the MTA (`CoIncrementMTAUsage`) for the whole session.
- Check PID liveness before activation; fail with `MREC_ERR_NO_PROCESSES`.
- Initialise clients with an explicit float32 format and `AUTOCONVERTPCM`.
- 1 s client buffers; time-critical drain threads.
- Idle loopback targets deliver continuous silent packets.
- Keep a source's packets contiguous within 20 ms of timestamp jitter, unless flagged `DATA_DISCONTINUITY`.
- Never sum a source with its own audio.
- Process targets add processes of the same conferencing app or Store package outside their tree.
- Drop targets inside another target's tree; two clients would double their audio.
- `E_NOTFOUND` → no microphone. `E_ACCESSDENIED` → `MREC_ERR_PERMISSION`.
- Mic `default`: OS default input. `auto`: capture device holding the targets' sessions, else communications default. Recheck every 500 ms.
- Mic: opt out of ducking (`SetDuckingPreference`).

## Meeting detection

- Rules shared by both backends: `native/common/meeting-catalog.c` (apps, browsers, URL/title patterns, scores).
- Backends supply OS facts: bundle ids or executables, titles, URLs, audio activity.
- Score: app 40 + output 30 + mic 25 + URL/title 5. Record at 70.
- Decide from audio IO; never scrape UI controls.
- Watchers: STARTED after ~2 s detected, ENDED after ~3 s missing (shared constants).
- Browsers: need mic activity or a recognised URL or title.
- `Meet - abc-defg-hij` titles match without a URL.
- Process targets: native code expands app PIDs. Windows follows the process tree plus sibling processes (same conferencing app or package), and adds siblings started later, such as a call host, within a second; macOS refreshes helper membership every second.
- Windows: attribute each audio session to its window-owning app (WebView2 → host app); one meeting per browser instance.
- Windows: a native meeting's `pid` is the lowest window-owning owner of its audio, stable across window changes.
- Windows: UI Automation reads address bars and tab strips; 250 ms call timeout, 500 ms walk, node budget, never inside documents.
- Windows: background tab titles count too; only the active tab has a URL.
- Windows app picker: nearest window-owning ancestor, never the shell; tray apps use their same-executable ancestor.

## Accessibility (AX)

- Set a ~250 ms messaging timeout.
- Set `AXEnhancedUserInterface` for Chromium and Electron trees.
- Cap traversal depth and node count.

## macOS permissions

- Audio prompt needs `NSAudioCaptureUsageDescription`, code signing, a GUI process, and a request off the main thread.
- No preflight API: probe with a throwaway tap; cache the result.
- Microphone: separate grant; `NSMicrophoneUsageDescription`; audio-input entitlement when sandboxed.

## Teardown

- Never SIGKILL a process holding a tap. Destroy the tap and aggregate device first.
