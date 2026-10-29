#!/usr/bin/env bash
# Isolated, real-server tests. Does not change the user's audio configuration.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${MREC_E2E_DBUS:-}" ]]; then
  exec dbus-run-session -- env MREC_E2E_DBUS=1 bash "$0" "$@"
fi
SERVER="${1:-pulse}"
BUILD="${MREC_LINUX_BUILD:-$ROOT/native/build/linux}"
bash "$ROOT/scripts/test-linux.sh"
RUNTIME="$(mktemp -d)"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME" PULSE_RUNTIME_PATH="$RUNTIME/pulse"
export PULSE_SERVER="unix:$RUNTIME/pulse/native"
mkdir -p "$PULSE_RUNTIME_PATH"
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$RUNTIME"
}
trap cleanup EXIT
if [[ "$SERVER" == pulse ]]; then
  pulseaudio -n --daemonize=no --use-pid-file=no --exit-idle-time=-1 --log-target="file:$RUNTIME/pulse.log" \
    --load="module-native-protocol-unix socket=$RUNTIME/pulse/native auth-anonymous=1" &
  pids+=("$!")
elif [[ "$SERVER" == pipewire ]]; then
  pipewire > "$RUNTIME/pipewire.log" 2>&1 & pids+=("$!")
  wireplumber > "$RUNTIME/wireplumber.log" 2>&1 & pids+=("$!")
  pipewire-pulse > "$RUNTIME/pulse.log" 2>&1 & pids+=("$!")
else
  echo "usage: $0 [pulse|pipewire]" >&2; exit 2
fi
ready=0
for ((i=0; i<100; i++)); do
  if pactl info >/dev/null 2>&1; then ready=1; break; fi
  sleep 0.1
done
if [[ "$ready" == 0 ]]; then cat "$RUNTIME"/*.log; exit 1; fi
for sink in mrec_speaker mrec_other mrec_mic_sink mrec_mic2_sink; do
  pactl load-module module-null-sink "sink_name=$sink" rate=48000 >/dev/null
done
pactl load-module module-remap-source master=mrec_mic_sink.monitor source_name=mrec_input source_properties=device.description=MrecMicrophone >/dev/null
pactl load-module module-remap-source master=mrec_mic2_sink.monitor source_name=mrec_input2 source_properties=device.description=MrecMicrophone2 >/dev/null
pactl set-default-source mrec_input
cp "$BUILD/zoom" "$BUILD/other-tone"
"$BUILD/zoom" mrec_speaker 440 mrec_input & TEST_PID_A=$!; pids+=("$!")
"$BUILD/other-tone" mrec_other 880 & TEST_PID_B=$!; pids+=("$!")
"$BUILD/other-tone" mrec_mic_sink 660 & pids+=("$!")
"$BUILD/other-tone" mrec_mic2_sink 990 & pids+=("$!")
export TEST_PID_A TEST_PID_B MREC_LINUX_BUILD="$BUILD"
sleep 1
cd "$ROOT"
python3 e2e/linux-native.py
node e2e/linux-node.mjs
if [[ "${MREC_SKIP_RUST:-0}" != 1 ]]; then
  if [[ -z "${MREC_RUST_E2E:-}" ]]; then
    cargo build --locked -p meeting-record --example linux_e2e
    rust_target="$(cargo metadata --no-deps --format-version 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["target_directory"])')"
    MREC_RUST_E2E="$rust_target/debug/examples/linux_e2e"
  fi
  "$MREC_RUST_E2E" "$TEST_PID_B"
fi
if [[ "$SERVER" == pipewire ]]; then
  python3 e2e/linux-tone-data.py | pw-cat --playback --target=mrec_speaker --rate=48000 --channels=1 --format=f32 - &
  TEST_NATIVE_PW_PID=$!; pids+=("$!")
  export TEST_NATIVE_PW_PID
  sleep 0.5
  node e2e/linux-native-pipewire.mjs
fi
if [[ "$SERVER" == pulse ]]; then dbus-run-session -- python3 e2e/linux-recovery.py; fi
echo "Linux $SERVER E2E passed"
