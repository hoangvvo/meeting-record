# meeting-record

System-audio and microphone capture with meeting detection for macOS and Windows.

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

| Platform | Minimum version | Architectures |
| -------- | --------------- | ------------- |
| macOS    | 14.2            | arm64, x86-64 |
| Windows  | 10 build 19041  | arm64, x86-64 |

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

## Permissions

| Permission    | Needed for                       | macOS                                    | Windows                  |
| ------------- | -------------------------------- | ---------------------------------------- | ------------------------ |
| System Audio  | Audio capture                    | One-time prompt                          | Granted without a prompt |
| Microphone    | Optional microphone track        | One-time prompt                          | System privacy settings  |
| Accessibility | Optional meeting titles and URLs | System Settings; relaunch after granting | Granted without a prompt |

- Requests are asynchronous: use the binding's request-and-wait API or query status before capture.
- Windows: enable microphone access for desktop apps in Settings → Privacy & security → Microphone.

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

- Record when `shouldRecord` / `should_record` is true, including listen-only meetings.
- An idle conferencing app alone does not count as a call.
- Scan for current meetings or watch for `started`, `updated`, and `ended` events.
- The watcher confirms starts after about two seconds and endings after about three seconds of absence.
- Detection works without Accessibility; titles and URLs may be empty.
- Windows: a background browser meeting may have a title but no URL.

## Capture

- **Process target:** captures the selected app and its audio helpers.
- **System target:** captures application output across the system.
- **Microphone:** opt-in, delivered as a separate track.
- **Mixed output:** optional combined system audio and microphone stream.
- **App picker:** `meetings.audioApps()` in Node.js or `meetings::audio_apps()` in Rust. Pass the entry's `pid` as a process target; no meeting scan is required.
- **Sessions:** one capture session at a time per process.

| Output       | Format                          | Channels                                      | Sample rate                                   |
| ------------ | ------------------------------- | --------------------------------------------- | --------------------------------------------- |
| System audio | Interleaved float32 PCM         | Mono by default; set `mono: false` for stereo | Negotiated capture rate                       |
| Microphone   | Float32 PCM                     | Mono                                          | Input device rate on macOS; 48 kHz on Windows |
| Mixed audio  | Signed 16-bit little-endian PCM | Mono                                          | Requested mix rate                            |

- **Raw audio:** each track has its own timing and format metadata. Read the sample rate and channel count from each packet.
- **Node output:** raw bytes are in `chunk.pcm`; mixed output is a byte stream.
- **Microphone selection:** `default` follows the OS input. `auto` prefers the captured app's mic on Windows, then the communications default; macOS uses its default input.
- **Channels:** `mono` affects system audio only.
- **Pause/resume:** apply to the recording; paused time is omitted.
- **Stop:** drain the outputs afterward to consume the final audio.
- **Processing:** no echo cancellation is applied.
- **Source loss:** requested sources must be available at startup. Later device loss keeps the session alive while the affected track recovers.
- **Availability:** `live`, `degraded` (some sources missing), or `interrupted` (none available). The microphone also reports device changes.
- **App exit:** if all captured apps exit, system audio stops but the microphone and session remain alive. A relaunched app requires a new capture session.

## Troubleshooting

| Symptom                                             | Fix                                                       |
| --------------------------------------------------- | --------------------------------------------------------- |
| Capture start times out on macOS                    | Request permissions from a signed GUI app.                |
| Capture is silent after the target exits            | Select a running app and start a new session.             |
| Titles or URLs are empty on macOS                   | Grant Accessibility permission, then relaunch.            |
| "none of the requested pids are running" on Windows | Rescan and select a running app.                          |
| "no default microphone is available" on Windows     | Enable an input device or capture without the microphone. |

## Development

```sh
npm test
cargo test --workspace
npm run lint
npm run fmt:check
```

Live audio tests on macOS:

```sh
MREC_IDENTITY="$(security find-identity -v -p codesigning | awk '/Developer ID Application/ { print $2; exit }')" ./e2e/macos.sh
```

Live audio tests on Windows:

```powershell
./e2e/windows.ps1                                         # system, process, app exit; both bindings
./e2e/windows.ps1 -Binding Rust                           # one binding
./e2e/windows.ps1 -MicrophoneDevice "CABLE Input"         # loopback microphone
./e2e/windows.ps1 -SwitchMicrophone "Stereo Mix"          # default-input switching
./e2e/windows.ps1 -RemoveMicrophone "CABLE Output"        # microphone loss and recovery
./e2e/windows.ps1 -SwitchOutput "Realtek Digital Output"  # default-output switching
./e2e/windows.ps1 -Mode Meeting -MeetingPid 1234          # a live meeting
```

See [internals](./docs/INTERNALS.md) for backend details.

## License

[MIT](./LICENSE.md) © Hoang Vo
