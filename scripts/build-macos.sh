#!/usr/bin/env bash
#
# Build the macOS native core (and optionally the C self-test app bundle).
#
#   ./scripts/build-macos.sh              # static library only
#   ./scripts/build-macos.sh selftest     # + signed .app that exercises it
#
# The self-test has to live in a bundle: CoreAudio process taps are gated by
# kTCCServiceAudioCapture, and TCC will not prompt for (or grant to) a bare
# executable with no Info.plist and no code-signing identity.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/native/build"   # not $ROOT/build: node-gyp owns that
mkdir -p "$BUILD"

# Codesigning identity. Ad-hoc ("-") is enough to get a TCC prompt for a local
# build; override with MREC_IDENTITY="Developer ID Application: ..." to ship.
IDENTITY="${MREC_IDENTITY:--}"

# Fails the build if meeting-record-detect.h drifts from the offsets Swift hardcodes.
echo "==> struct layout check"
clang -I"$ROOT/native/include" -o "$BUILD/layout_check" "$ROOT/native/tests/layout_check.c"
"$BUILD/layout_check"

echo "==> swiftc: libmeetingrecord_macos.a"
swiftc -O \
  -parse-as-library \
  -emit-library -static \
  -target arm64-apple-macos14.2 \
  -o "$BUILD/libmeetingrecord_macos.a" \
  "$ROOT"/native/macos/*.swift

echo "    $(du -h "$BUILD/libmeetingrecord_macos.a" | cut -f1) $BUILD/libmeetingrecord_macos.a"

# Detection needs no entitlements or bundle, so it builds as a plain binary.
echo "==> classification tests"
swiftc -O -o "$BUILD/catalog_test" \
  "$ROOT/native/tests/catalog_test.swift" \
  "$ROOT"/native/macos/{MeetingCatalog,MeetingDetector,Accessibility,AudioProcesses}.swift
"$BUILD/catalog_test" | tail -2

echo "==> meetingtest"
clang -O2 \
  -I"$ROOT/native/include" \
  -o "$BUILD/meetingtest" \
  "$ROOT/native/examples/meetingtest.c" \
  "$BUILD/libmeetingrecord_macos.a" \
  -framework CoreAudio -framework AVFoundation -framework AudioToolbox \
  -framework Foundation -framework AppKit -framework ApplicationServices \
  -L"$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx" \
  -lswiftCore -lm
echo "    built $BUILD/meetingtest"

[[ "${1:-}" == "selftest" ]] || exit 0

APP="$BUILD/MrecSelfTest.app"
echo "==> selftest app bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

clang -O2 \
  -I"$ROOT/native/include" \
  -o "$APP/Contents/MacOS/MrecSelfTest" \
  "$ROOT/native/examples/selftest.c" \
  "$ROOT/native/examples/selftest_main.m" \
  "$BUILD/libmeetingrecord_macos.a" \
  -framework CoreAudio -framework AVFoundation -framework AudioToolbox \
  -framework Foundation -framework AppKit \
  -L"$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx" \
  -lswiftCore -lm

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>MrecSelfTest</string>
  <key>CFBundleIdentifier</key><string>dev.meetingrecord.selftest</string>
  <key>CFBundleName</key><string>MrecSelfTest</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSMinimumSystemVersion</key><string>14.2</string>
  <key>LSUIElement</key><true/>
  <key>NSAudioCaptureUsageDescription</key>
  <string>Records system audio so meetings can be transcribed.</string>
  <key>NSMicrophoneUsageDescription</key>
  <string>Records your microphone so meetings can be transcribed.</string>
</dict></plist>
PLIST

cat > "$BUILD/meeting-record.entitlements" <<'ENT'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.device.audio-input</key><true/>
</dict></plist>
ENT

codesign --force --options runtime \
  --entitlements "$BUILD/meeting-record.entitlements" \
  --sign "$IDENTITY" "$APP"
xattr -cr "$APP"

echo "    built $APP (identity: $IDENTITY)"
echo
echo "Run it with:  open $APP     # then read /tmp/meeting-record-selftest.log"
echo "Play audio first, or it will correctly report that nothing is playing."
