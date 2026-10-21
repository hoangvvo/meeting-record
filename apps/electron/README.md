# mrec-electron

Node binding test app. macOS 14.2+ (arm64) or Windows 10 build 19041+.

Build the core first, from the repo root:

```sh
npm run build
```

Dev:

```sh
npm start --workspace meeting-record-electron
```

Build and run the real thing:

```sh
MREC_IDENTITY="$(security find-identity -v -p codesigning | awk '/Apple Development/ { print $2; exit }')" \
  npm run package --workspace meeting-record-electron
open out/mrec-electron-darwin-arm64/mrec-electron.app
```

Notes:

- macOS dev mode: detection only; no permission prompts, no recording.
- Windows dev mode: detection and recording.
- Windows build: needs Python and the Visual Studio C++ build tools.
