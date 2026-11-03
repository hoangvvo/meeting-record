# mrec-tauri

Rust binding test app. macOS 14.2+ (arm64), Windows 11 / Server 2022+, or Linux (see the root requirements).

Run from the repository root:

```sh
npm run build --workspace meeting-record
```

```sh
npm start --workspace meeting-record-tauri
```

## Package for macOS

```sh
APPLE_SIGNING_IDENTITY="$(security find-identity -v -p codesigning | awk '/Apple Development/ { print $2; exit }')" \
  npm run package --workspace meeting-record-tauri
open "$(git rev-parse --show-toplevel)/target/release/bundle/macos/mrec-tauri.app"
```

| Platform        | Development mode        |
| --------------- | ----------------------- |
| macOS           | Detection only          |
| Windows / Linux | Detection and recording |

Linux builds require the [Tauri prerequisites](https://v2.tauri.app/start/prerequisites/#linux) and [SDK development libraries](../../README.md#requirements).
