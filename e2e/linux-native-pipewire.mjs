import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import { capture, meetings } from "../node/dist/index.js";
const pid = Number(process.env.TEST_NATIVE_PW_PID);
const apps = meetings.audioApps();
assert(
  apps.some((app) => app.pid === pid && app.isPlayingAudio),
  "native PipeWire client is not exposed through the compatibility server",
);
const recording = await capture.start({ type: "process", pid });
const samples = [];
recording.systemAudio.on("data", (chunk) => {
  assert.equal(chunk.droppedSamples, 0);
  for (let i = 0; i < chunk.frameCount; i++) samples.push(chunk.pcm.readFloatLE(i * 4));
});
await delay(2000);
await recording.stopRecording();
assert(samples.length > 60000, `native PipeWire capture missing: ${samples.length} frames`);
let re = 0,
  im = 0,
  energy = 0;
for (let i = samples.length - 48000; i < samples.length; i++) {
  const angle = (2 * Math.PI * 550 * i) / 48000;
  re += samples[i] * Math.cos(angle);
  im += samples[i] * Math.sin(angle);
  energy += samples[i] ** 2;
}
assert(
  (2 * (re ** 2 + im ** 2)) / 48000 / energy > 0.7,
  "native PipeWire tone missing or unrelated audio leaked in",
);
console.log("native PipeWire playback attribution and isolated capture passed");
