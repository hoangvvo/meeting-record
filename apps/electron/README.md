# mrec-electron

Node binding test app. macOS 14.2+, arm64.

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

- Dev mode can't do permissions or recording. TCC reads Electron.app's plist, so no prompt ever shows up. Detection works fine.
