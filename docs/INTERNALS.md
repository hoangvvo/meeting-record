# Internals

Tested on macOS 26.5.1 (Apple silicon) and Windows 11 build 26200 (x86-64).
Linux: Ubuntu 24.04 x86-64 under WSL2; real PulseAudio/PipeWire servers, Chrome WebRTC on X11/Wayland, and two signed-in Chrome participants in live Meet on X11. Controlled remote speech is checked against separate system PCM and the mixed WAV. Ubuntu 22.04, Debian 12, and Fedora 44 pass native compilation and shared tests; a glibc-tagged Ubuntu 22.04 Node prebuild loads on all three. Other providers, physical hotplug/Bluetooth, and arm64 hardware remain unverified.

## Backends

| Platform | System audio                           | Microphone                                              |
| -------- | -------------------------------------- | ------------------------------------------------------- |
| macOS    | CoreAudio process tap (14.2+)          | `AVAudioEngine`, default input                          |
| Windows  | WASAPI process loopback (build 20348+) | WASAPI shared mode, default input or automatic call mic |
| Linux    | `libpulse` per-sink-input monitors     | `libpulse`, default input or captured app's input       |

- No virtual audio drivers. No ScreenCaptureKit.
- Linux requires the current user's PulseAudio-compatible server, local `/proc` process identity, and AT-SPI for UI evidence. The same code serves distributions with those interfaces; direct ALSA/JACK and sandboxed hosts are outside the supported boundary.
- Linux API references: [per-stream monitoring](https://freedesktop.org/software/pulseaudio/doxygen/stream_8h.html), [PipeWire's PulseAudio replacement](https://docs.pipewire.org/page_man_pipewire-pulse_1.html), and [AT-SPI](https://gnome.pages.gitlab.gnome.org/at-spi2-core/libatspi/). [Recall's OS matrix](https://docs.recall.ai/docs/dsdk-supported-platforms) does not list Linux; the [community Granola port](https://github.com/tirtha4/Granola-for-Linux) reports limited system audio.
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
- Linux: one monitor per playback stream, with the shared timeline mixer and 200 ms alignment headroom. Drain every fragment in each notification. Both tracks use 48 kHz float32. Anchor the sample clock to client receipt minus all queued frames; preserve continuity until reads stall or a device/server is replaced. Pace callbacks, including final drain, until their sample timestamp is current. [Pulse timing estimates](https://www.freedesktop.org/software/pulseaudio/doxygen/structpa__timing__info.html) are in the device clock domain and can be stale or jump; using them to reposition contiguous PCM caused lost audio and misaligned mixes in live WSLg tests. Linux timestamps do not compensate hardware input/playback latency.
- Linux: poll graph/default-device changes every 250 ms; reconnect after service loss. Target identity includes `/proc` start time; only descendants of the original instance join capture. Unknown local PIDs are excluded, and global capture excludes the recorder's process tree.
- Source loss, shared by all backends: `native/common/tracks.c`. Recording never stops for it.
- Lost source: reopen at once (published as a device change), else `INTERRUPTED` and retry every 500 ms until it returns, then `RESUMED`. A device notification retries at once (Windows `IMMNotificationClient`, macOS default-input and default-output listeners); stop ends pending retries at once.
- Microphone: `DEVICE_CHANGED` on a new device. Events are serialized and never arrive after stop.
- Every captured app exited → system `INTERRUPTED` (`APP_EXITED`); stop system callbacks and retries, and keep the microphone/session alive. Do not follow a relaunched app. Windows matches captured processes by PID and creation time, so a process that reuses a PID is not mistaken for the app.
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

- Rules shared by all backends: `native/common/meeting-catalog.c` (apps, browsers, URL/title patterns, scores).
- Backends supply OS facts: bundle ids or executables, titles, URLs, audio activity.
- Score: app 40 + output 30 + mic 25 + URL/title 5. Record at 70.
- Joined controls supersede audio activity; pre-join/lobby/ended controls veto it. Unknown UI uses conservative audio evidence.
- Meet exposes camera-less previews as `Join now without camera` and `Switch here without camera`; these also veto active microphone/playback evidence.
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
- Linux: serialize AT-SPI cache access on a private GLib context; never dispatch the host GUI's default context from a scan worker. Use bounded Collection queries to find browser documents, and inspect controls only inside a recognized top-level service document. Nested documents never inherit the host's call identity. Unrelated document URLs cannot fall back to stale window titles. Chromium exposes the URL as the document's `URI` attribute.
- Linux: a live Meet listen-only call with no microphone activity can disappear from detection when its tab is hidden. Returning to the tab restores detection. Manual process capture continues independently; playback alone must not become a browser-call fallback.

## macOS permissions

- Audio prompt needs `NSAudioCaptureUsageDescription`, code signing, a GUI process, and a request off the main thread.
- No preflight API: probe with a throwaway tap; cache the result.
- Microphone: separate grant; `NSMicrophoneUsageDescription`; audio-input entitlement when sandboxed.

## Teardown

- Never SIGKILL a process holding a tap. Destroy the tap and aggregate device first.
