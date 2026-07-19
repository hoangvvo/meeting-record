# mrec-tauri

Rust binding test app. macOS 14.2+, arm64.

Build the core first, from the repo root:

```sh
npm run build
```

Dev:

```sh
npm start --workspace meeting-record-tauri
```

Build and run the real thing:

```sh
APPLE_SIGNING_IDENTITY="Apple Development: NAME (TEAM)" \
  npm run package --workspace meeting-record-tauri
open target/release/bundle/macos/mrec-tauri.app
```

`security find-identity -v -p codesigning` gives you the identity string. `cargo tauri build` is the same tool if you have cargo-tauri installed.

Notes:

- Dev mode can't do permissions or recording. The dev binary is unsigned so TCC won't prompt. Detection works fine.
- The audio callback only memcpys into an `rtrb` ring. A normal thread drains it, writes the WAV, and emits level and elapsed at 10Hz.
