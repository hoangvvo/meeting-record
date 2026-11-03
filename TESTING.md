# Testing

## Requirements

- Node.js 22.13+ (22.x) or 24+, npm, stable Rust, Python 3, and a C++17 compiler.
- Meet and Zoom accounts, a second participant on another device, and Chrome. Add Firefox and Safari for the browser checks below, and Teams on Windows.
- Speakers, a headset or USB microphone, and two selectable inputs and outputs. The missing-input check needs a machine with removable or disableable inputs.
- [Electron](apps/electron/README.md) or [Tauri](apps/tauri/README.md) for manual recording. Microphone effects and mixed output use the library's capture options.

| Platform       | Setup                                                                                                                                                 |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| macOS          | macOS 14.2+, Xcode tools, a signed app, audio/microphone access, and Accessibility access. Use a packaged example app for recording.                  |
| Windows        | Windows 11 / Server 2022+, Visual Studio C++ build tools, PowerShell, and microphone access for desktop apps. Device removal needs an elevated shell. |
| Linux build    | `pkg-config`, `libpulse-dev`, `libatspi2.0-dev`.                                                                                                      |
| Linux E2E      | A graphical session, D-Bus, AT-SPI, PulseAudio, `pactl`, PipeWire, `pipewire-pulse`, WirePlumber, and `pw-cat`.                                       |
| Linux browsers | Chrome, Firefox, geckodriver, OpenSSL, `xvfb-run`, `xauth`, and Weston. Ports 9223, 9443, and 4445 free.                                              |

## Unit tests

From the project directory:

```sh
npm ci
npm test
cargo test -p meeting-record
```

| Platform | Additional native tests                                             |
| -------- | ------------------------------------------------------------------- |
| Windows  | `powershell -ExecutionPolicy Bypass -File scripts/test-windows.ps1` |
| Linux    | `bash scripts/test-linux.sh`                                        |

## E2E

These tests play audio and record the microphone.

### macOS

```sh
MREC_TEST_MICROPHONE=1 bash e2e/macos.sh
```

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode All -Binding All -Microphone
```

For device changes, replace the names below with devices from the two lists:

```powershell
powershell -ExecutionPolicy Bypass -File e2e/windows-audio-device.ps1 -Action List
powershell -ExecutionPolicy Bypass -File e2e/windows-audio-device.ps1 -Output -Action List
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode Process -Binding Node -SwitchMicrophone "SECOND INPUT"
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode Process -Binding Node -RemoveMicrophone "REMOVABLE INPUT"
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode Process -Binding Node -SwitchOutput "SECOND OUTPUT"
```

### Linux

```sh
bash e2e/linux.sh pulse
bash e2e/linux.sh pipewire
MREC_BROWSER=chrome bash e2e/linux-browser.sh x11
MREC_BROWSER=chrome bash e2e/linux-browser.sh wayland
MREC_BROWSER=firefox bash e2e/linux-browser.sh x11
MREC_BROWSER=firefox bash e2e/linux-browser.sh wayland
```

Set `CHROME`, `FIREFOX`, or `GECKODRIVER` if the executable is outside `PATH`.

## Manual checks

| Platform | Full call sequence | Detection and a short recording |
| -------- | ------------------ | ------------------------------- |
| macOS    | Zoom, Chrome Meet  | Firefox Meet, Safari Meet       |
| Windows  | Zoom, Chrome Meet  | Teams, Firefox Meet             |
| Linux    | Zoom, Chrome Meet  | —                               |

### Calls

| Action                                                                                                        | Expected result                                                                                   |
| ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| Open the home screen, call preview, or waiting room.                                                          | No meeting start; `shouldRecord` is false.                                                        |
| Join a call and exchange speech.                                                                              | Correct app, PID, and platform; one `started` event.                                              |
| Mute the call microphone with call controls visible.                                                          | Call remains detected.                                                                            |
| Record the app while another app plays audio.                                                                 | System track contains remote speech and excludes the other app. Microphone contains local speech. |
| Pause for 3 seconds, resume, stop, and start another recording.                                               | No paused time, repeated audio, or missing final words. The next recording starts.                |
| Leave the call and wait up to 10 seconds.                                                                     | `shouldRecord` becomes false; one `ended` event.                                                  |
| Stop and restart the watcher during a call. On Windows, also stop from a callback.                            | Events resume without duplicates; no events after watcher stop.                                   |
| On Windows, start Teams capture before joining.                                                               | Call audio is captured when the call starts.                                                      |
| In Slack or Viber, send a message, play a voice note, then record a voice note with and without app playback. | No meeting start or recording prompt outside a call.                                              |

### Devices and mixed output

Use Meet or Zoom with physical audio devices. For Linux, check both PulseAudio and PipeWire.

| Action                                                                                                                | Expected result                                                                                               |
| --------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Select different system and call inputs; capture with `Default`, then `Auto`.                                         | Recorded microphone and reported name match the system input for `Default` and the call input for `Auto`.     |
| Start without an input, then connect one.                                                                             | System capture continues; microphone changes from interrupted to resumed with a valid format and device name. |
| Start without an input, stop, then connect one.                                                                       | No recording restart or callbacks after stop.                                                                 |
| Deny microphone access on macOS/Windows, or audio-server access on Linux; attempt capture, restore access, and retry. | First attempt returns a permission error; retry succeeds.                                                     |
| Switch input and output during recording.                                                                             | System capture continues; microphone recovers with the new device name.                                       |
| With each binding, record a 16 kHz mix, mute only the mixed microphone, pause/resume, stop, and drain.                | Mono s16le at 16 kHz; raw microphone still available; final audio retained.                                   |

### Microphone effects

Compare a recording with effects off against one with echo cancellation, noise suppression, and gain control enabled. Set `microphoneProcessing` in Node or `microphone_processing` in Rust.

| Action                                                                    | Expected result                                                                    |
| ------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Play remote speech through local speakers while remaining silent locally. | Less remote echo in the microphone; clear system audio.                            |
| Speak quietly, then normally, with steady background noise.               | Clear local speech, less noise, and increased quiet-speech level without clipping. |
| Pause/resume, replace the microphone, speak, then stop and drain.         | No repeated frames, missing final words, or increasing track offset.               |

### macOS permissions and accessibility

| Action                                                                                     | Expected result                                                              |
| ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------- |
| Deny system-audio access, attempt capture, grant access, and retry.                        | First attempt fails; retry records.                                          |
| Deny Accessibility, scan, grant access, and relaunch the app.                              | Scan works without titles; browser detection works after relaunch.           |
| Read Chrome's accessibility flag before watching, during scanning, and after watcher stop. | A previously false flag returns to false; a previously true flag stays true. |
| With the watcher stopped, scan once and wait 65 seconds without another scan.              | A flag enabled by the scan returns to false.                                 |
| Move, resize, or tile Chrome with macOS window controls after watcher stop.                | Normal window controls work.                                                 |

Read the flag from a terminal with Accessibility access, using Chrome's PID:

```sh
swift -e 'import ApplicationServices; var value: CFTypeRef?; let app = AXUIElementCreateApplication(pid_t(CommandLine.arguments[1])!); let status = AXUIElementCopyAttributeValue(app, "AXEnhancedUserInterface" as CFString, &value); print(status.rawValue, value as Any)' "$APP_PID"
```
