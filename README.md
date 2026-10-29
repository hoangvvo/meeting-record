# meeting-record

System-audio and microphone capture with meeting detection for macOS, Windows, and Linux.

- Capture one app or all system audio, with an optional microphone track.
- Read raw audio or a combined mono stream.
- Detect active meetings and watch for changes.
- Keep recording through device loss and recover when the source returns.

## Packages

- [Node.js / TypeScript](./node/README.md)
- [Rust](./rust/README.md)

## Examples

![Demo app](apps/electron/screenshots/demo.png)

- [Electron](./apps/electron/README.md) — Node.js binding
- [Tauri](./apps/tauri/README.md) — Rust binding

Need an AI Note-taker? Check out [fireflies.ai](https://fireflies.ai/?utm_source=github&utm_medium=referral&utm_campaign=hoangvvo_meeting-record).

## Requirements

| Platform             | Minimum version                              | Architectures |
| -------------------- | -------------------------------------------- | ------------- |
| macOS                | 14.2                                         | arm64, x86-64 |
| Windows              | 11 / Server 2022                             | arm64, x86-64 |
| Linux (experimental) | PulseAudio or PipeWire with `pipewire-pulse` | arm64, x86-64 |

macOS app requirements:

- Signed and running as a GUI app.
- `NSAudioCaptureUsageDescription` in `Info.plist`.
- `NSMicrophoneUsageDescription` only when capturing the microphone.

```xml
<key>NSAudioCaptureUsageDescription</key>
<string>Records meeting audio so it can be transcribed.</string>
<key>NSMicrophoneUsageDescription</key>
<string>Records your voice so meetings can be transcribed.</string>
```

- The example apps include the required configuration.
- Windows native builds need Python and the Visual Studio C++ build tools.
- Linux: `libpulse` and `libatspi` runtime libraries; Node prebuilds require glibc 2.35+. Source builds need a C++17 compiler, Python, `pkg-config`, and the development headers (`libpulse-dev libatspi2.0-dev` on Debian/Ubuntu; `pulseaudio-libs-devel at-spi2-core-devel` on Fedora).
- Linux uses one backend across glibc-based distributions. ALSA/JACK-only sessions and sandboxed host apps are not supported. Desktop audio services must be available in the current user session.

## Permissions

| Permission    | Needed for                       | macOS                                    | Windows                  | Linux               |
| ------------- | -------------------------------- | ---------------------------------------- | ------------------------ | ------------------- |
| System Audio  | Audio capture                    | One-time prompt                          | Granted without a prompt | Audio server access |
| Microphone    | Optional microphone track        | One-time prompt                          | System privacy settings  | Audio server access |
| Accessibility | Optional meeting titles and URLs | System Settings; relaunch after granting | Granted without a prompt | AT-SPI desktop bus  |

- Requests are asynchronous: use the binding's request-and-wait API or query status before capture.
- Windows: enable microphone access for desktop apps in Settings → Privacy & security → Microphone.
- Linux has no portable permission dialog. Audio status checks the server connection; source access is checked at capture start. Enable desktop accessibility for UI detection; Chromium may also need `--force-renderer-accessibility` and `ACCESSIBILITY_ENABLED=1` when launching the browser.

## Detection

| ID        | Name                         |
| --------- | ---------------------------- |
| `zoom`    | Zoom                         |
| `teams`   | Microsoft Teams              |
| `meet`    | Google Meet                  |
| `webex`   | Webex                        |
| `slack`   | Slack                        |
| `discord` | Discord                      |
| `browser` | Other browser-based meetings |

- Record when `shouldRecord` / `should_record` is true. The library combines the evidence and makes this recommendation; consumers do not need to interpret call states or UI controls.
- Classification and confidence rules are shared across backends. Audio activity is combined only within the same app instance.
- Recognised in-call controls yield confidence 90–100, including silent/listen-only calls and Teams web calls on `/v2/`. Recognised pre-join, lobby, and ended-screen controls suppress detection even while the microphone is active.
- Call checks cover Teams, Meet, Zoom, Webex, Slack huddles, Discord voice connections, and recognised browser services. Stable accessibility identifiers are preferred; exact English control/status labels provide additional matches. UI inspection is bounded and scoped to the relevant app window or service document.
- When call UI is unavailable or unrecognised, detection falls back to microphone activity from the meeting app. Browsers also need a recognised call URL or tab title. Playback alone, Teams home/chat pages, and microphone use on unrelated sites do not qualify through this fallback.
- Scan for current meetings or watch for `started`, `updated`, and `ended` events.
- The watcher confirms starts after about two seconds and endings after about three seconds of absence.
- Native app audio-based detection works without Accessibility. On macOS, Accessibility enables call-state checks and browser detection; relaunch after granting it.
- Windows: a background browser meeting may have a title but no URL.
- Linux: AT-SPI supplies call controls and document URLs on X11 and Wayland. A hidden listen-only tab can disappear from detection; an existing capture continues. Missing controls use conservative audio fallback. Native Linux provider clients remain unverified.
- The watcher also emits updates when confidence changes. Detection remains a heuristic: UI changes, unsupported labels/languages, inaccessible controls, and browser-wide audio attribution can force a less precise fallback. Manual capture remains available independently of detection.

## Capture

- **Process target:** captures the selected app and its audio helpers.
- **System target:** captures application output across the system.
- **Microphone:** opt-in, delivered as a separate track.
- **Mixed output:** optional combined system audio and microphone stream.
- **App picker:** `meetings.audioApps()` in Node.js or `meetings::audio_apps()` in Rust. Pass the entry's `pid` as a process target; no meeting scan is required.
- **Sessions:** one capture session at a time per process.

| Output       | Format                          | Channels                                      | Sample rate                                         |
| ------------ | ------------------------------- | --------------------------------------------- | --------------------------------------------------- |
| System audio | Interleaved float32 PCM         | Mono by default; set `mono: false` for stereo | Negotiated capture rate                             |
| Microphone   | Float32 PCM                     | Mono                                          | Input device rate on macOS; 48 kHz on Windows/Linux |
| Mixed audio  | Signed 16-bit little-endian PCM | Mono                                          | Requested mix rate                                  |

- **Raw audio:** each track has its own timing and format metadata. Read the sample rate and channel count from each packet.
- **Node output:** raw bytes are in `chunk.pcm`; mixed output is a byte stream.
- **Microphone selection:** `default` follows the OS input. `auto` prefers the captured app's mic on Windows/Linux, then the default (communications default on Windows); macOS uses its default input.
- **Channels:** `mono` affects system audio only.
- **Pause/resume:** apply to the recording; paused time is omitted.
- **Stop:** drain the outputs afterward to consume the final audio.
- **Processing:** no echo cancellation is applied.
- **Source loss:** requested sources must be available at startup. Later device loss keeps the session alive while the affected track recovers.
- **Availability:** `live`, `degraded` (some sources missing), or `interrupted` (none available). The microphone also reports device changes.
- **App exit:** if all captured apps exit, system audio stops but the microphone and session remain alive. A relaunched app requires a new capture session.
- **Linux:** monitors app playback across outputs with 200 ms buffering. Timestamps follow PCM receipt on a shared monotonic clock; device latency is not compensated. Audio bypassing the PulseAudio-compatible server and streams without a local PID are excluded. Browser targeting includes other audible tabs in that instance.

## Troubleshooting

| Symptom                                             | Fix                                                                                       |
| --------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| Capture start times out on macOS                    | Request permissions from a signed GUI app.                                                |
| Capture is silent after the target exits            | Select a running app and start a new session.                                             |
| Titles or URLs are empty on macOS                   | Grant Accessibility permission, then relaunch.                                            |
| "none of the requested pids are running" on Windows | Rescan and select a running app.                                                          |
| "no default microphone is available" on Windows     | Enable an input device or capture without the microphone.                                 |
| Linux cannot connect to the audio server            | Start PulseAudio or `pipewire-pulse` in the same user session.                            |
| Linux browser meetings are not detected             | Enable AT-SPI accessibility in the desktop and browser; manual capture remains available. |

## Development

```sh
npm test
cargo test --workspace
npm run lint
npm run fmt:check
```

Linux native and isolated audio/WebRTC tests:

```sh
bash e2e/linux.sh pulse
bash e2e/linux.sh pipewire
bash e2e/linux-browser.sh x11
bash e2e/linux-browser.sh wayland
MREC_BROWSER=firefox bash e2e/linux-browser.sh x11
MREC_BROWSER=firefox bash e2e/linux-browser.sh wayland
```

- Linux E2E needs Rust, PulseAudio, PipeWire, WirePlumber, `pactl`, D-Bus, AT-SPI, Python, Xvfb, Weston, OpenSSL, and Chrome or Firefox with geckodriver (`CHROME`, `FIREFOX`, and `GECKODRIVER` override executables). Run after building the Node package; both bindings run by default (`MREC_SKIP_RUST=1` skips Rust). Tests use private audio servers and local WebRTC without changing desktop audio settings.
- Set `MREC_STRESS_SECONDS=180` for sustained capture and 50 capture restarts alongside meeting scans. `MREC_SANITIZERS=address,undefined bash scripts/test-linux.sh` runs the native checks with sanitizers.
- For real calls, run `node e2e/linux-live.mjs BROWSER_PID` in the browser's Linux desktop/audio session. Commands: `mark LABEL`, `start LABEL`, `pause`, `resume`, `stop`, `finish`. Saves detection events, PCM metrics, separate tracks, and a mixed WAV under `native/build/linux/artifacts/`.
- Linux desktop examples: Electron and Tauri UI recording, separate tracks, mixing, and WAV export are tested under WSLg.
- Verified on Ubuntu 24.04 x86-64 under WSL2 with PulseAudio/PipeWire, Chrome/Firefox WebRTC on X11/Wayland, and live Chrome Meet calls on X11. CI runs audio and Firefox tests on native x64/arm64 runners. Native builds pass on Ubuntu 22.04, Debian 12, and Fedora 44; the same Ubuntu 22.04 Node prebuild loads on all three. Other real providers and physical device hotplug/Bluetooth remain unverified.

Live audio tests on macOS:

```sh
MREC_IDENTITY="$(security find-identity -v -p codesigning | awk '/Developer ID Application/ { print $2; exit }')" ./e2e/macos.sh
```

Live audio tests on Windows:

```powershell
./e2e/windows.ps1                                                 # every scenario, both bindings
./e2e/windows.ps1 -Binding Rust                                   # every scenario, one binding
./e2e/windows.ps1 -SwitchMicrophone "Stereo Mix"                  # choose a device instead of detecting one
./e2e/windows.ps1 -Mode All                                       # system, process, app exit, and helpers only
./e2e/windows.ps1 -Mode Process -RemoveMicrophone "CABLE Output"  # one scenario
./e2e/windows.ps1 -Mode Meeting -MeetingPid 1234                  # a live meeting
```

Without `-Mode`, the script detects devices: a [VB-Audio cable](https://vb-audio.com/Cable/)
for the microphone tone and microphone removal, and other active devices for the
input and output switches. Scenarios without a device are skipped.

See [internals](./docs/INTERNALS.md) for backend details.

## License

[MIT](./LICENSE.md) © Hoang Vo
