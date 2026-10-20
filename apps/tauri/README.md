# mrec-tauri

Rust binding test app. macOS 14.2+ (arm64) or Windows 10 build 20348+.

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
- Windows dev mode: detection and recording.
- The audio callback copies into an `rtrb` ring; a normal thread writes the WAV and emits level and elapsed at 10 Hz.
