#!/usr/bin/env bash
#
# Build the macOS native core, and optionally the C self-test app bundle.
#
#   ./scripts/build-macos.sh              # static library only
#   ./scripts/build-macos.sh selftest     # + signed .app that exercises it
#
# The self-test needs a bundle: TCC will not prompt for a bare executable with no
# Info.plist and no code-signing identity.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/native/build"   # not $ROOT/build: node-gyp owns that
mkdir -p "$BUILD"

# Codesigning identity; override with MREC_IDENTITY to ship.
IDENTITY="${MREC_IDENTITY:--}"

ARCH="${MREC_ARCH:-$(uname -m)}"
case "$ARCH" in
  arm64) SWIFT_TARGET="arm64-apple-macos14.2" ;;
  x86_64) SWIFT_TARGET="x86_64-apple-macos14.2" ;;
  *) echo "unsupported macOS architecture: $ARCH" >&2; exit 1 ;;
esac

# Fails if meeting-record-detect.h drifts from the offsets Swift hardcodes.
echo "==> struct layout check"
clang -I"$ROOT/native/include" -o "$BUILD/layout_check" "$ROOT/native/tests/layout_check.c"
"$BUILD/layout_check"

echo "==> application process-tree tests"
clang++ -std=c++17 -Wall -Wextra -Werror \
  -o "$BUILD/audio_app_process_test" "$ROOT/native/tests/audio_app_process_test.cpp"
"$BUILD/audio_app_process_test"

echo "==> meeting catalog tests"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/native/include" -I"$ROOT/native/common" \
  -o "$BUILD/meeting_catalog_test" "$ROOT/native/tests/meeting_catalog_test.c" \
  "$ROOT/native/common/meeting-catalog.c"
"$BUILD/meeting_catalog_test"

echo "==> track recovery tests"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/native/include" -I"$ROOT/native/common" \
  -o "$BUILD/tracks_test" "$ROOT/native/tests/tracks_test.c" "$ROOT/native/common/tracks.c"
"$BUILD/tracks_test"

echo "==> track concurrency tests"
clang -std=c11 -Wall -Wextra -Werror -I"$ROOT/native/include" -I"$ROOT/native/common" \
  -c -o "$BUILD/tracks-test.o" "$ROOT/native/common/tracks.c"
clang++ -std=c++17 -Wall -Wextra -Werror -pthread \
  -I"$ROOT/native/include" -I"$ROOT/native/common" \
  -o "$BUILD/tracks_concurrency_test" "$ROOT/native/tests/tracks_concurrency_test.cpp" \
  "$BUILD/tracks-test.o"
"$BUILD/tracks_concurrency_test"

echo "==> Windows capture pump tests"
clang++ -std=c++17 -Wall -Wextra -Werror -pthread \
  -I"$ROOT/native/include" -I"$ROOT/native/common" \
  -o "$BUILD/capture_pump_test" "$ROOT/native/tests/capture_pump_test.cpp" \
  "$BUILD/tracks-test.o"
"$BUILD/capture_pump_test"

# The Windows stream mixer is portable C++.
echo "==> Windows timeline mixer tests"
clang++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT/native/include" \
  -o "$BUILD/timeline_mixer_test" "$ROOT/native/tests/timeline_mixer_test.cpp"
"$BUILD/timeline_mixer_test"

# Rules shared with the Windows backend; Swift imports them through Bridging.h.
BRIDGING_HEADER="$ROOT/native/macos/Bridging.h"
for source in meeting-catalog tracks; do
  clang -O2 -std=c11 -Wall -Wextra -Werror -target "$SWIFT_TARGET" -I"$ROOT/native/include" \
    -c -o "$BUILD/$source.o" "$ROOT/native/common/$source.c"
done

echo "==> swiftc: libmeetingrecord_macos.a"
swiftc -O \
  -parse-as-library \
  -emit-library -static \
  -target "$SWIFT_TARGET" \
  -import-objc-header "$BRIDGING_HEADER" \
  -o "$BUILD/libmeetingrecord_macos.a" \
  "$ROOT"/native/macos/*.swift
ar rs "$BUILD/libmeetingrecord_macos.a" "$BUILD/meeting-catalog.o" "$BUILD/tracks.o"

echo "    $(du -h "$BUILD/libmeetingrecord_macos.a" | cut -f1) $BUILD/libmeetingrecord_macos.a"

# Detection needs no entitlements or bundle.
echo "==> classification tests"
swiftc -O -target "$SWIFT_TARGET" -import-objc-header "$BRIDGING_HEADER" \
  -o "$BUILD/catalog_test" \
  "$ROOT/native/tests/catalog_test.swift" \
  "$ROOT"/native/macos/{MeetingCatalog,MeetingDetector,Accessibility,AudioProcesses,ProcessTree,BrowserContent}.swift \
  "$BUILD/meeting-catalog.o"
"$BUILD/catalog_test" | tail -2

echo "==> browser detection tests"
swiftc -O -parse-as-library -target "$SWIFT_TARGET" -import-objc-header "$BRIDGING_HEADER" \
  -o "$BUILD/browser_detection_test" \
  "$ROOT/native/tests/browser_detection_test.swift" \
  "$ROOT"/native/macos/{MeetingCatalog,MeetingDetector,Accessibility,AudioProcesses,ProcessTree,BrowserContent}.swift \
  "$BUILD/meeting-catalog.o"
"$BUILD/browser_detection_test"

echo "==> tap description tests"
swiftc -O -parse-as-library -target "$SWIFT_TARGET" -import-objc-header "$BRIDGING_HEADER" \
  -o "$BUILD/tap_description_test" \
  "$ROOT/native/tests/tap_description_test.swift" \
  "$ROOT/native/macos/"{TapCapture,CaptureState,AudioProcesses,ProcessTree}.swift
"$BUILD/tap_description_test" | tail -2

echo "==> capture buffer tests"
swiftc -O -parse-as-library -target "$SWIFT_TARGET" \
  -o "$BUILD/capture_state_test" \
  "$ROOT/native/tests/capture_state_test.swift" "$ROOT/native/macos/CaptureState.swift"
"$BUILD/capture_state_test"

echo "==> capture lifecycle tests"
swiftc -O -parse-as-library -target "$SWIFT_TARGET" -import-objc-header "$BRIDGING_HEADER" \
  -o "$BUILD/capture_lifecycle_test" \
  "$ROOT/native/tests/capture_test_support.swift" \
  "$ROOT/native/tests/capture_lifecycle_test.swift" \
  "$ROOT/native/macos/Exports.swift" "$BUILD/tracks.o"
"$BUILD/capture_lifecycle_test"

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
  "$ROOT/native/examples/exclusiontest.m" \
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
echo "Run it with:  $APP/Contents/MacOS/MrecSelfTest"
echo "Set MREC_TEST_MICROPHONE=1 to validate the microphone track too."
