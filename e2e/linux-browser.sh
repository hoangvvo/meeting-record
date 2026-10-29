#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${MREC_LINUX_BUILD:-$ROOT/native/build/linux}"
MODE="${1:-x11}"
if [[ -z "${MREC_E2E_DBUS:-}" ]]; then
  if [[ "$MODE" == x11 ]]; then
    exec dbus-run-session -- env MREC_E2E_DBUS=1 xvfb-run -a bash "$0" "$MODE"
  fi
  exec dbus-run-session -- env MREC_E2E_DBUS=1 bash "$0" "$MODE"
fi
RUNTIME="$(mktemp -d)"
bash "$ROOT/scripts/test-linux.sh"
export XDG_RUNTIME_DIR="$RUNTIME" PULSE_RUNTIME_PATH="$RUNTIME/pulse" PULSE_SERVER="unix:$RUNTIME/pulse/native"
mkdir -p "$PULSE_RUNTIME_PATH"
pulseaudio -n --daemonize=no --use-pid-file=no --exit-idle-time=-1 --log-target="file:$RUNTIME/pulse.log" \
  --load="module-native-protocol-unix socket=$RUNTIME/pulse/native auth-anonymous=1" \
  --load="module-null-sink sink_name=mrec_speaker rate=48000" \
  --load="module-null-sink sink_name=mrec_mic_sink rate=48000" \
  --load="module-remap-source master=mrec_mic_sink.monitor source_name=mrec_input" & pulse_pid=$!
chrome_pid=""
fixture_pid=""
tone_pid=""
weston_pid=""
cleanup() {
  local status=$?
  [[ -z "$chrome_pid" ]] || kill -- "-$chrome_pid" 2>/dev/null || true
  [[ -z "$fixture_pid" ]] || kill "$fixture_pid" 2>/dev/null || true
  [[ -z "$tone_pid" ]] || kill "$tone_pid" 2>/dev/null || true
  [[ -z "$weston_pid" ]] || kill "$weston_pid" 2>/dev/null || true
  kill "$pulse_pid" 2>/dev/null || true
  wait 2>/dev/null || true
  if [[ "$status" != 0 ]]; then
    mkdir -p "$ROOT/native/build/linux/artifacts/browser-$MODE"
    cp "$RUNTIME"/*.log "$ROOT/native/build/linux/artifacts/browser-$MODE/" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME"
}
trap cleanup EXIT
CHROME="${CHROME:-google-chrome}"
ozone=()
chrome_sandbox=()
if [[ "$(id -u)" == 0 ]]; then chrome_sandbox+=(--no-sandbox); fi
if [[ "$MODE" == wayland ]]; then
  weston --backend=headless --renderer=pixman --no-config --idle-time=0 --socket=wayland-mrec > "$RUNTIME/weston.log" 2>&1 & weston_pid=$!
  export WAYLAND_DISPLAY=wayland-mrec
  for ((i=0; i<100; ++i)); do [[ ! -S "$RUNTIME/$WAYLAND_DISPLAY" ]] || break; sleep 0.1; done
  ozone+=(--ozone-platform=wayland)
elif [[ "$MODE" == x11 ]]; then ozone+=(--ozone-platform=x11)
else echo "usage: $0 [x11|wayland]" >&2; exit 2
fi
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$RUNTIME/key.pem" -out "$RUNTIME/cert.pem" -days 1 -subj /CN=meet.google.com > /dev/null 2>&1
python3 "$ROOT/e2e/linux-fixture-server.py" "$RUNTIME" "$ROOT/e2e/linux-fixture.html" > "$RUNTIME/fixture.log" 2>&1 & fixture_pid=$!
for ((i=0; i<100; ++i)); do if pactl info > /dev/null 2>&1; then break; fi; sleep 0.1; done
"$BUILD/zoom" mrec_mic_sink 660 & tone_pid=$!
export PULSE_SINK=mrec_speaker PULSE_SOURCE=mrec_input
ACCESSIBILITY_ENABLED=1 setsid "$CHROME" "${chrome_sandbox[@]}" --no-first-run --no-default-browser-check --disable-dev-shm-usage \
  --force-renderer-accessibility --user-data-dir="$RUNTIME/chrome" \
  "${ozone[@]}" \
  --use-fake-ui-for-media-stream --autoplay-policy=no-user-gesture-required \
  --ignore-certificate-errors --no-proxy-server '--host-resolver-rules=MAP meet.google.com 127.0.0.1, MAP unrelated.example 127.0.0.1' \
  --remote-debugging-port=9223 --remote-debugging-address=127.0.0.1 \
  'https://meet.google.com:9443/abc-defg-hij' > "$RUNTIME/chrome.log" 2>&1 & chrome_pid=$!
export TEST_BROWSER_PID="$chrome_pid"
cd "$ROOT"
if [[ -n "${MREC_DEBUG_ACCESSIBILITY:-}" ]]; then
  sleep 2
  python3 e2e/linux-accessibility-dump.py
  native/build/linux/diagnose
fi
node e2e/linux-browser.mjs
echo "Linux $MODE Chrome WebRTC E2E passed"
