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
MREC_IDENTITY="Apple Development: NAME (TEAM)" \
  npm run package --workspace meeting-record-electron
open apps/electron/out/mrec-electron-darwin-arm64/mrec-electron.app
```

`security find-identity -v -p codesigning` gives you the identity string.

Notes:

- Dev mode can't do permissions or recording. TCC reads Electron.app's plist, so no prompt ever shows up. Detection works fine.
- That leaves `node/build/Release/*.node` on Electron's ABI. Run `npm run build:node` before going back to `node/examples/*.mjs`.
