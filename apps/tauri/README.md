# mrec-tauri

Rust binding test app. macOS 14.2+ (arm64), Windows 11 / Server 2022+, or Linux (experimental; see the root requirements).

Build the core first:

```sh
npm run build --workspace meeting-record
```

Dev:

```sh
npm start --workspace meeting-record-tauri
```

Build and run the real thing:

```sh
APPLE_SIGNING_IDENTITY="$(security find-identity -v -p codesigning | awk '/Apple Development/ { print $2; exit }')" \
  npm run package --workspace meeting-record-tauri
open "$(git rev-parse --show-toplevel)/target/release/bundle/macos/mrec-tauri.app"
```

`cargo tauri build` works too, with cargo-tauri installed.

Notes:

- macOS dev mode: detection only; no permission prompts, no recording.
- Windows/Linux dev mode: detection and recording.
- Linux: install the [Tauri prerequisites](https://v2.tauri.app/start/prerequisites/#linux) and the SDK development libraries. `npm run package --workspace meeting-record-tauri` builds a Debian package; revealing a recording opens its parent folder with `xdg-open`.
