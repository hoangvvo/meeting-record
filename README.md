# meeting-record

System-audio and microphone capture with meeting detection for macOS and Windows. Records call audio without virtual audio devices, kernel extensions, or screen recording.

## Packages

- [Node.js / TypeScript](./node/README.md)
- [Rust](./rust/README.md)

## Platform support

| Platform | Minimum version | Architectures |
| -------- | --------------- | ------------- |
| macOS    | 14.2            | arm64, x86-64 |
| Windows  | 10 build 20348  | arm64, x86-64 |

On macOS, system-audio capture needs `NSAudioCaptureUsageDescription`. Add `NSMicrophoneUsageDescription` only when requesting the microphone track. The app must be signed and running as a GUI application for permission prompts.

```xml
<key>NSAudioCaptureUsageDescription</key>
<string>Records meeting audio so it can be transcribed.</string>
<key>NSMicrophoneUsageDescription</key>
<string>Records your voice so meetings can be transcribed.</string>
```

## Permissions

| Permission        | Purpose                                                  | Prompt type            |
| ----------------- | -------------------------------------------------------- | ---------------------- |
| **System Audio**  | Required to capture audio.                               | One-time modal dialog. |
| **Microphone**    | Required only when the microphone track is requested.    | One-time modal dialog. |
| **Accessibility** | Optional. Required only to read meeting titles and URLs. | System Settings.       |

Permission handling is asynchronous. Query the permission status for the result.

Microphone capture is opt-in. It uses the current default input device and stays separate from system audio; the library never mixes the two tracks together.

## Detection

The passive watcher reports meetings starting, changing, and ending on a safe serial queue.

| ID        | Name                         |
| --------- | ---------------------------- |
| `zoom`    | Zoom                         |
| `teams`   | Microsoft Teams              |
| `meet`    | Google Meet                  |
| `webex`   | Webex                        |
| `slack`   | Slack                        |
| `discord` | Discord                      |
| `browser` | Other browser-based meetings |

Always rely on `shouldRecord` / `should_record`. It becomes true when there is active two-way audio or remote audio; idle conferencing apps remain false.

## Capture

- **Process target:** Recommended. Captures the selected meeting process and its audio helpers. Continues capturing if the user mutes their system speakers.
- **System target:** Captures the whole system mix, but records silence if system output is muted.

## Troubleshooting

- **Capture start times out:** The permission grant is undetermined. Request permissions from a bundled GUI app, not a bare CLI executable.
- **Capture runs but outputs zeroes:** The system target was used while output was muted, or the targeted processes exited.
- **Titles/URLs are empty:** Accessibility permission was not granted, or the app was not restarted after granting it.
- **Audio is broken machine-wide on macOS:** A crash or force-quit orphaned a process tap. Run `sudo killall coreaudiod` to reset it.
