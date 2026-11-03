# Testing

## Requirements

| Target     | Requirements                                                                                                                                                                                                                                                         |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| All        | Node.js 22.13+ (22.x) or 24+, npm, stable Rust, Python 3, C++17 compiler. Run commands from the repository root.                                                                                                                                                     |
| macOS      | macOS 14.2+, Xcode command-line tools, a signed capture host with audio/microphone usage descriptions, system-audio and microphone grants, Accessibility grant. Use packaged examples for recording.                                                                 |
| Windows    | Windows 11 or Server 2022+, Visual Studio C++ build tools, PowerShell, desktop microphone access enabled. Device-disable tests require an elevated shell.                                                                                                            |
| Linux      | A graphical user session, D-Bus, AT-SPI, `pkg-config`, `libpulse-dev`, `libatspi2.0-dev`. For E2E: PulseAudio, `pactl`, PipeWire, `pipewire-pulse`, WirePlumber, `pw-cat`. For browser fixtures: Chrome, Firefox, geckodriver, OpenSSL, `xvfb-run`, `xauth`, Weston. |
| Live calls | Google Meet and Zoom accounts, a second participant on another device, Chrome and Firefox. Add Safari on macOS and Teams on Windows. Install one messaging app, such as Slack or Viber.                                                                              |
| Devices    | Speakers, a headset or USB microphone, two selectable inputs and outputs. Use a test machine or VM with removable/disableable inputs for the no-microphone case.                                                                                                     |
| Test host  | [Electron](apps/electron/README.md) or [Tauri](apps/tauri/README.md). For configurable mixed output and microphone effects, use the [Node](node/README.md) or [Rust](rust/README.md) capture options and save both tracks plus mixed output.                         |

## Automated checks

Run once on each shipped OS/architecture:

```sh
npm ci
npm test
cargo test -p meeting-record
```

Run the additional native checks on Windows or Linux:

| Platform | Command                                                             |
| -------- | ------------------------------------------------------------------- |
| Windows  | `powershell -ExecutionPolicy Bypass -File scripts/test-windows.ps1` |
| Linux    | `bash scripts/test-linux.sh`                                        |

These checks run without audio playback or microphone recording.

## Automated E2E

These tests play audio and record the microphone. Close other calls before running them.

### macOS

```sh
MREC_TEST_MICROPHONE=1 bash e2e/macos.sh
```

Grant the self-test app audio and microphone access when prompted. The script exits with status `0` when capture, source-exit, and recorder-exclusion checks pass.

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode All -Binding All -Microphone
```

The run checks system capture, process capture, source exit, helper processes, pause/resume, and restart for both bindings.

List device names, then run each device transition once with the Node binding:

```powershell
powershell -ExecutionPolicy Bypass -File e2e/windows-audio-device.ps1 -Action List
powershell -ExecutionPolicy Bypass -File e2e/windows-audio-device.ps1 -Output -Action List
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode Process -Binding Node -SwitchMicrophone "SECOND INPUT"
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode Process -Binding Node -RemoveMicrophone "REMOVABLE INPUT"
powershell -ExecutionPolicy Bypass -File e2e/windows.ps1 -Mode Process -Binding Node -SwitchOutput "SECOND OUTPUT"
```

The checks should pass and restore the original devices. Skip a device check if the named device is unavailable.

### Linux

```sh
bash e2e/linux.sh pulse
bash e2e/linux.sh pipewire
MREC_BROWSER=chrome bash e2e/linux-browser.sh x11
MREC_BROWSER=chrome bash e2e/linux-browser.sh wayland
MREC_BROWSER=firefox bash e2e/linux-browser.sh x11
MREC_BROWSER=firefox bash e2e/linux-browser.sh wayland
```

Set `CHROME`, `FIREFOX`, or `GECKODRIVER` when executables are outside `PATH`. Ports 9223, 9443, and 4445 must be free.

## Live-call checks

Run the full sequence with Zoom and Chrome on each OS. For the other apps and browsers listed below, check detection and record a short sample of remote speech.

| Platform | Desktop apps | Browsers                |
| -------- | ------------ | ----------------------- |
| macOS    | Zoom         | Chrome, Firefox, Safari |
| Windows  | Zoom, Teams  | Chrome, Firefox         |
| Linux    | Zoom         | Chrome                  |

| Step                                                             | Required result                                                                                                                 |
| ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| Open the app home screen, then the call preview or waiting room. | `shouldRecord` is false; watcher does not start a meeting.                                                                      |
| Join with the second participant and exchange speech.            | Scan reports the correct app/PID and platform; watcher emits one `started` event.                                               |
| Mute the call microphone with call controls visible.             | The joined call remains detected.                                                                                               |
| Capture the app while a different app plays audio.               | System track contains the remote participant and excludes the other app. Microphone track contains local speech.                |
| Pause for 3 seconds, resume, stop, then drain outputs.           | Output excludes the pause, retains the final words, and contains no duplicated segment. A second recording starts successfully. |
| Leave the call and wait up to 10 seconds.                        | Scan stops recommending recording; watcher emits one `ended` event.                                                             |
| Stop and restart the watcher during a call.                      | Events resume without duplicates; no callbacks arrive after stop.                                                               |

On Windows, repeat watcher stop from inside a callback. With Teams, start capture before joining and verify that call audio appears when the call starts.

In the messaging app, send a message, receive a notification, play a voice note, and record a voice note. Repeat recording a voice note while that app plays audio. None of these actions should start a meeting or show a recording prompt.

Test other apps when their detection or audio routing changes.

## Microphone and mixed output

Run these checks once per OS with Chrome Meet or Zoom. Record both tracks and the mixed output.
Skip device-transition rows already passed by the automated E2E run.

| Check                | Operation                                                                                                      | Required result                                                                                                                     |
| -------------------- | -------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Default input        | Select different system-default and call inputs; capture with `Default`.                                       | Reported name and recorded microphone match the system default.                                                                     |
| App input            | Capture with `Auto`, then change the call input.                                                               | Reported name and local speech follow the call input; unavailable app input falls back to the default.                              |
| Missing input        | Grant access, disable/disconnect all inputs, start capture, then reconnect one.                                | System capture starts and continues; mic reports interrupted, then resumes with its device name and valid format.                   |
| Stop during recovery | Start without an input and stop before reconnecting it.                                                        | Stop completes promptly; reconnecting does not restart capture or emit callbacks.                                                   |
| Permission denied    | On macOS/Windows, deny microphone access and request it. On Linux, deny the test client's audio-server access. | Startup fails with a permission error; a subsequent permitted capture succeeds.                                                     |
| Device replacement   | Switch input and output during recording.                                                                      | System capture continues; mic changes or interrupts/resumes as needed. No stale device name or duplicated audio.                    |
| Mixed output         | Record at 16 kHz; mute only the mixed microphone, pause/resume, then stop and drain.                           | Mono s16le at the requested rate; raw microphone stays available; mixed microphone is muted; final audio and duration are retained. |

Run mixed output once through each binding. Repeat device checks on Linux with PulseAudio and PipeWire. Repeat input replacement during a recording with all microphone effects enabled.

## Microphone effects

Configure the host using `microphoneProcessing` in Node or `microphone_processing` in Rust. Record one baseline with effects off, then repeat with echo cancellation, noise suppression, and automatic gain control all enabled.

| Operation                                                                               | Required result                                                                                                |
| --------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| Remote participant speaks through local speakers while the local participant is silent. | Processed microphone has less remote echo; captured system speech remains clear.                               |
| Local participant speaks quietly, then normally, with steady background noise.          | Local speech remains intelligible, background noise is reduced, and quiet speech gains level without clipping. |
| Pause/resume, replace the microphone, speak, then stop and drain.                       | No stale echo, repeated frames, missing final words, or growing track offset.                                  |

Run the live effects check once per OS.

## macOS accessibility and permissions

| Operation                                                                                          | Required result                                                                                              |
| -------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| Deny system-audio access, attempt capture, then grant access and retry.                            | First attempt fails cleanly; retry records successfully.                                                     |
| Deny Accessibility, scan, then grant it and relaunch the host.                                     | Scan remains usable without titles; browser detection works after relaunch.                                  |
| Check Chrome's `AXEnhancedUserInterface` before watching, during scanning, and after watcher stop. | A previously false flag returns to false after stop. A flag already enabled before watching remains enabled. |
| With the watcher stopped, run one scan, then wait 65 seconds without scanning again.               | A flag enabled by the scan returns to false.                                                                 |
| Move/resize Chrome with Rectangle or Magnet after watcher stop.                                    | Normal window-management behavior resumes.                                                                   |

Read the accessibility flag with an Accessibility-authorized terminal:

```sh
swift -e 'import ApplicationServices; var value: CFTypeRef?; let app = AXUIElementCreateApplication(pid_t(CommandLine.arguments[1])!); let status = AXUIElementCopyAttributeValue(app, "AXEnhancedUserInterface" as CFString, &value); print(status.rawValue, value as Any)' "$APP_PID"
```

## Results

Record the OS/version/architecture, binding, app/browser versions, audio server, device names, commands, outcome, and artifact paths. Save track events and raw/mixed audio for failures.
