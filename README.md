# meeting-record

System-audio and microphone capture with meeting detection for macOS, Windows, and Linux.

## Packages

| Package                                  | Version                                                                                                                                                                                            | Example                               |
| ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------- |
| [Node.js / TypeScript](./node/README.md) | [![npm](https://img.shields.io/npm/v/meeting-record)](https://www.npmjs.com/package/meeting-record)                                                                                                | [Electron](./apps/electron/README.md) |
| [Rust](./rust/README.md)                 | [![crates.io](https://img.shields.io/crates/v/meeting-record)](https://crates.io/crates/meeting-record) [![docs.rs](https://img.shields.io/docsrs/meeting-record)](https://docs.rs/meeting-record) | [Tauri](./apps/tauri/README.md)       |

## Requirements

| Platform             | Minimum version                              | Architectures |
| -------------------- | -------------------------------------------- | ------------- |
| macOS                | 14.2                                         | arm64, x86-64 |
| Windows              | 11 / Server 2022                             | arm64, x86-64 |
| Linux (experimental) | PulseAudio or PipeWire with `pipewire-pulse` | arm64, x86-64 |

- Requires Node.js 22.13+ (22.x) or 24+.
- On macOS, use a signed GUI app with `NSAudioCaptureUsageDescription` in `Info.plist`. Add `NSMicrophoneUsageDescription` when capturing the microphone.
- Windows source builds require Python and the Visual Studio C++ build tools.
- Linux requires `libpulse` and `libatspi`; Node prebuilds require glibc 2.35+. Source builds also need a C++17 compiler, Python, `pkg-config`, `libpulse-dev`, and `libatspi2.0-dev`.

## Permissions

Request system-audio permission before capture and microphone permission when including the microphone.

| Permission        | macOS                              | Windows                                   | Linux                                    |
| ----------------- | ---------------------------------- | ----------------------------------------- | ---------------------------------------- |
| System audio      | Grant when requested               | Available                                 | Running audio server                     |
| Microphone        | Grant when requested               | Enable microphone access for desktop apps | Running audio server                     |
| Browser detection | Grant Accessibility, then relaunch | Available                                 | Enable desktop and browser accessibility |

## Detection

Use `shouldRecord` / `should_record` to decide when to record. Scan for meetings or watch for `started`, `updated`, and `ended` events.

| Desktop app       | macOS | Windows | Linux |
| ----------------- | :---: | :-----: | :---: |
| 8x8 Work          |   ✓   |    ✓    |   —   |
| Aircall           |   ✓   |    ✓    |   —   |
| Aircall Workspace |   ✓   |    ✓    |   —   |
| ClickUp           |   ✓   |    ✓    |   —   |
| Dialpad           |   ✓   |    ✓    |   —   |
| Dialpad Meetings  |   ✓   |    ✓    |   —   |
| Discord           |   ✓   |    ✓    |   ✓   |
| Element           |   ✓   |    ✓    |   ✓   |
| Feishu            |   ✓   |    —    |   ✓   |
| Gather            |   ✓   |    ✓    |   —   |
| Gather V2         |   ✓   |    ✓    |   —   |
| GoTo              |   ✓   |    ✓    |   —   |
| Jami              |   ✓   |    ✓    |   ✓   |
| Jitsi Meet        |   ✓   |    ✓    |   ✓   |
| Lark              |   ✓   |    ✓    |   ✓   |
| LINE              |   ✓   |    ✓    |   —   |
| Mattermost        |   ✓   |    ✓    |   ✓   |
| Microsoft Teams   |   ✓   |    ✓    |   —   |
| Nextcloud Talk    |   ✓   |    ✓    |   ✓   |
| RingCentral       |   ✓   |    ✓    |   —   |
| Roam              |   ✓   |    ✓    |   —   |
| Signal            |   ✓   |    ✓    |   ✓   |
| Slack             |   ✓   |    ✓    |   ✓   |
| Telegram          |   ✓   |    ✓    |   ✓   |
| Tencent Meeting   |   ✓   |    ✓    |   ✓   |
| Tuple             |   ✓   |    ✓    |   ✓   |
| Viber             |   ✓   |    ✓    |   ✓   |
| VooV Meeting      |   ✓   |    ✓    |   —   |
| Webex             |   ✓   |    ✓    |   ✓   |
| WeChat            |   ✓   |    ✓    |   ✓   |
| WeCom             |   ✓   |    ✓    |   —   |
| WhatsApp          |   ✓   |    ✓    |   —   |
| Yandex Telemost   |   ✓   |    ✓    |   —   |
| Zalo              |   ✓   |    ✓    |   —   |
| Zoom              |   ✓   |    ✓    |   ✓   |

| Browser        | macOS | Windows | Linux |
| -------------- | :---: | :-----: | :---: |
| Arc            |   ✓   |    ✓    |   —   |
| Brave          |   ✓   |    ✓    |   ✓   |
| Chrome         |   ✓   |    ✓    |   ✓   |
| Chromium       |   ✓   |    ✓    |   ✓   |
| Comet          |   ✓   |    ✓    |   —   |
| Dia            |   ✓   |    —    |   —   |
| Edge           |   ✓   |    ✓    |   ✓   |
| Firefox        |   ✓   |    ✓    |   ✓   |
| Helium         |   ✓   |    ✓    |   ✓   |
| Naver Whale    |   ✓   |    —    |   ✓   |
| Opera          |   ✓   |    ✓    |   ✓   |
| Opera Air      |   ✓   |    —    |   —   |
| Opera GX       |   ✓   |    —    |   —   |
| Orion          |   ✓   |    —    |   —   |
| Safari         |   ✓   |    —    |   —   |
| Vivaldi        |   ✓   |    ✓    |   ✓   |
| Waterfox       |   ✓   |    ✓    |   ✓   |
| Yandex Browser |   ✓   |    ✓    |   —   |
| Zen            |   ✓   |    ✓    |   ✓   |

Browser calls are recognised for 8x8, Around, Dialpad Meetings, Discord, Gather, GoTo, Google Meet, Jitsi Meet, Microsoft Teams, RingCentral, Slack, Tuple, Webex, Whereby, Yandex Telemost, and Zoom.

## Capture

| Output       | Format                          | Channels                         | Sample rate           |
| ------------ | ------------------------------- | -------------------------------- | --------------------- |
| System audio | Interleaved float32 PCM         | Mono by default; optional stereo | Read from each packet |
| Microphone   | Float32 PCM                     | Mono                             | Read from each packet |
| Mixed audio  | Signed 16-bit little-endian PCM | Mono                             | Configurable          |

- Select an app with `meetings.audioApps()` in Node.js or `meetings::audio_apps()` in Rust, then pass its `pid` as the capture target.
- One recording can run at a time per process.
- Stop the session, then drain the outputs to keep the final audio.
- Capturing a browser may include audio from other tabs in the same instance.

## Troubleshooting

| Problem                        | Action                                                                      |
| ------------------------------ | --------------------------------------------------------------------------- |
| Capture cannot start           | Check permissions and select a running app. On macOS, use a signed GUI app. |
| Microphone unavailable         | Enable an input device or capture without the microphone.                   |
| Browser meetings are missing   | Enable Accessibility on macOS or desktop/browser accessibility on Linux.    |
| Linux audio server unavailable | Start PulseAudio or `pipewire-pulse` in the current user session.           |

## License

[MIT](./LICENSE.md) © Hoang Vo
