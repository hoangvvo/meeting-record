# meeting-record

System-audio and microphone capture with meeting detection for macOS and Windows.

## Packages

- [Node.js / TypeScript](./node/README.md)
- [Rust](./rust/README.md)

## Examples

![Demo app](apps/electron/screenshots/demo.png)

- [Tauri](./apps/tauri/README.md) (Rust binding)
- [Electron](./apps/electron/README.md) (Node.js binding)

Need an AI Note-taker? Check out [fireflies.ai](https://fireflies.ai/?utm_source=github&utm_medium=referral&utm_campaign=hoangvvo_meeting-record).

## Platform support

| Platform | Minimum version | Architectures |
| -------- | --------------- | ------------- |
| macOS    | 14.2            | arm64, x86-64 |
| Windows  | 10 build 19041  | arm64, x86-64 |

macOS app requirements:

- Signed, running as a GUI app.
- `NSAudioCaptureUsageDescription` in `Info.plist`.
- `NSMicrophoneUsageDescription` only for the microphone track.

```xml
<key>NSAudioCaptureUsageDescription</key>
<string>Records meeting audio so it can be transcribed.</string>
<key>NSMicrophoneUsageDescription</key>
<string>Records your voice so meetings can be transcribed.</string>
```

## Permissions

| Permission        | Needed for                    | macOS prompt           |
| ----------------- | ----------------------------- | ---------------------- |
| **System Audio**  | Any capture                   | One-time modal dialog. |
| **Microphone**    | The microphone track          | One-time modal dialog. |
| **Accessibility** | Meeting titles and URLs (opt) | System Settings.       |

- Requests are asynchronous: request, then poll the status.
- Windows: no prompts. System Audio and Accessibility report `granted`.
- Windows: Microphone reports Settings › Privacy & security › Microphone; `denied` there → capture start fails with a permission error.

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
- The watcher reports `started`, `updated` and `ended` on a serial queue.
- `started` needs ~2 s of detection; `ended` ~3 s of absence.
- Windows: a Meet call in a background tab is `meet` with an empty `url`.

## Capture

- **Process target:** recommended; the meeting app and its helpers.
- **System target:** all application audio.
- **Microphone:** opt-in. Windows: the mic the captured app uses, else the communications default. macOS: the default input.
- A lost source never stops a recording: its track is interrupted and retried until it returns.
- Tracks report `interrupted` (with a reason) and `resumed`; the microphone reports `devicechange`.
- Every captured app exiting interrupts system audio (`app-exited`).
- **Mixed output:** optional mono s16le from both bindings.
- Source pickers: [`meetings.audioApps()`](./node/README.md#audio-apps) (Node.js), [`meetings::audio_apps()`](./rust/README.md#audio-apps) (Rust).

## Troubleshooting

| Symptom                                        | Fix                                                           |
| ---------------------------------------------- | ------------------------------------------------------------- |
| Capture start times out (macOS)                | Request permissions from a bundled GUI app.                   |
| Capture outputs zeroes                         | The target exited; rescan.                                    |
| Titles/URLs empty (macOS)                      | Grant Accessibility, then relaunch.                           |
| "none of the requested pids are running" (Win) | The target exited; rescan.                                    |
| "no default microphone is available" (Win)     | Enable a recording device, or capture without the microphone. |

## Development

Lint and format:

```sh
npm run lint:fix && npm run fmt
cargo +nightly clippy --all-targets --all-features --workspace && cargo +nightly fmt
```

Unit tests:

```sh
npm test
cargo test -p meeting-record --all-targets
```

macOS E2E:

```sh
MREC_IDENTITY="$(security find-identity -v -p codesigning | awk '/Developer ID Application/ { print $2; exit }')" ./e2e/macos.sh
```

Windows E2E (needs Python and the Visual Studio C++ build tools):

```powershell
./e2e/windows.ps1                                    # system + process, both bindings
./e2e/windows.ps1 -Binding Rust                      # one binding
./e2e/windows.ps1 -MicrophoneDevice "CABLE Input"    # + virtual-cable microphone
./e2e/windows.ps1 -SwitchMicrophone "Stereo Mix"     # + default-microphone switch
./e2e/windows.ps1 -Mode Meeting -MeetingPid 1234     # a live meeting
```
