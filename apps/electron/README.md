# mrec-electron

Node binding test app. macOS 14.2+ (arm64), Windows 11 / Server 2022+, or Linux (see the root requirements).

Run from the repository root:

```sh
npm run build
```

```sh
npm start --workspace meeting-record-electron
```

## Package for macOS

```sh
MREC_IDENTITY="$(security find-identity -v -p codesigning | awk '/Apple Development/ { print $2; exit }')" \
  npm run package --workspace meeting-record-electron
open out/mrec-electron-darwin-arm64/mrec-electron.app
```

| Platform        | Development mode        |
| --------------- | ----------------------- |
| macOS           | Detection only          |
| Windows / Linux | Detection and recording |
