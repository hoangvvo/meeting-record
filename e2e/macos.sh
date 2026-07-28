#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DURATION="${MREC_DURATION_SECONDS:-8}"
MINIMUM_PEAK="${MREC_MIN_PEAK:-0.0001}"

if [[ ! "$DURATION" =~ ^[0-9]+$ ]] || (( DURATION < 2 || DURATION > 30 )); then
  echo "MREC_DURATION_SECONDS must be an integer from 2 through 30" >&2
  exit 2
fi

TEMP_DIR="$(mktemp -d /tmp/meeting-record-e2e.XXXXXX)"
TONE_FILE="$TEMP_DIR/reference.aiff"
PLAYER_PID=""

cleanup() {
  if [[ -n "$PLAYER_PID" ]] && kill -0 "$PLAYER_PID" 2>/dev/null; then
    kill "$PLAYER_PID" 2>/dev/null || true
    wait "$PLAYER_PID" 2>/dev/null || true
  fi
  rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

"$ROOT/scripts/build-macos.sh" selftest

# A long local speech fixture avoids network access and gives the capture a
# deterministic non-silent source. System capture reads the app output directly.
say -r 100 -o "$TONE_FILE" \
  "Meeting record end to end validation. This reference audio checks system capture, timestamps, duration, and clean shutdown. The sentence continues long enough for the complete test run. Meeting record end to end validation. This reference audio checks system capture, timestamps, duration, and clean shutdown. The sentence continues long enough for the complete test run. Meeting record end to end validation. This reference audio checks system capture, timestamps, duration, and clean shutdown."

rm -f /tmp/meeting-record-selftest.log
afplay "$TONE_FILE" &
PLAYER_PID=$!
sleep 1

export MREC_GLOBAL=1
export MREC_DURATION_SECONDS="$DURATION"
export MREC_MIN_PEAK="$MINIMUM_PEAK"

APP="$ROOT/native/build/MrecSelfTest.app"
set +e
"$APP/Contents/MacOS/MrecSelfTest"
STATUS=$?
set -e

if [[ -f /tmp/meeting-record-selftest.log ]]; then
  cat /tmp/meeting-record-selftest.log
fi
exit "$STATUS"
