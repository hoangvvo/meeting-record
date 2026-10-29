import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import { capture, meetings, permissions, Permission } from "../node/dist/index.js";

// A desktop may have working audio without an accessibility bus. Repeated
// probes must fail safely rather than passing a null connection to libdbus.
assert(process.env.AT_SPI_BUS_ADDRESS);
for (let i = 0; i < 3; i++) assert.equal(permissions.status(Permission.Accessibility), "unknown");
const calls = await meetings.scan();
const pid = Number(process.env.TEST_PID_A);
assert(calls.some((call) => call.pid === pid && call.shouldRecord));
const session = await capture.start({ type: "process", pid });
const stats = { frames: 0 };
session.systemAudio.on("data", (chunk) => (stats.frames += chunk.frameCount));
try {
  const deadline = performance.now() + 5000;
  while (stats.frames < 48000 && performance.now() < deadline) await delay(50);
  assert(stats.frames >= 48000, "audio capture must work without AT-SPI");
} finally {
  await session.stopRecording();
}
console.log("missing accessibility bus preserves permission probes, audio fallback, and capture");
delete process.env.AT_SPI_BUS_ADDRESS;
// Desktop test installations include the bus launcher. A failed probe must not
// permanently poison libatspi's process-wide initialization state.
assert.equal(permissions.status(Permission.Accessibility), "granted");
console.log("accessibility initialization recovers after a failed bus probe");
