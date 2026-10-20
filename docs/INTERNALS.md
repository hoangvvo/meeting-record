# Internals

Tested on macOS 26.5.1 (Apple silicon) and Windows 11 build 26200 (x86-64).

## Backends

| Platform | System audio                           | Microphone                        |
| -------- | -------------------------------------- | --------------------------------- |
| macOS    | CoreAudio process tap (14.2+)          | `AVAudioEngine`, default input    |
| Windows  | WASAPI process loopback (build 20348+) | WASAPI shared mode, default input |

- No virtual audio drivers. No ScreenCaptureKit.
- macOS TCC state (`auth_value` 2 = allowed, 0 = denied):

```sql
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service,client,auth_value from access where service='kTCCServiceAudioCapture'"
```

## Pipeline

- Native callbacks: interleaved float32, first-frame timestamp on the host monotonic clock.
- Bindings: one fixed-size queue per track; coalesce compatible packets; no wake per callback.
- System and microphone: separate tracks and formats; pause, resume and stop together.
- macOS: process tap → private aggregate device (empty sub-device list) → `AudioDeviceCreateIOProcID`.
- macOS microphone: separate `AVAudioEngine` input tap.
- Windows: one loopback client per PID, aligned on QPC into one 10 ms track.
- Recovery: serialized, 3 retries, `RECOVERING` → `RUNNING` or terminal `FAILED`.
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
- Initialise loopback clients with an explicit float32 format.
- Idle loopback targets deliver continuous silent packets.
- Keep a source's packets contiguous within 20 ms of timestamp jitter, unless flagged `DATA_DISCONTINUITY`.
- Never sum a source with its own audio.
- `E_NOTFOUND` → no microphone. `E_ACCESSDENIED` → `MREC_ERR_PERMISSION`.

## Meeting detection

- Score: app 40 + output 30 + mic 25 + URL/title 5. Record at 70.
- Decide from audio IO; never scrape UI controls.
- Browsers: need mic activity or a recognised URL or title.
- Process targets: native code expands app PIDs. Windows follows the process tree; macOS snapshots helpers at start.
- Windows: attribute each audio session to its window-owning app (WebView2 → host app); one meeting per browser instance.
- Windows: UI Automation reads address bars and tab strips; 250 ms call timeout, 500 ms walk, node budget, never inside documents.
- Windows: `Meet - abc-defg-hij` matches in any tab; only the active tab has a URL.
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
