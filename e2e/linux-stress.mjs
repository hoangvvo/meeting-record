import assert from "node:assert/strict";
import { readdirSync, readFileSync, mkdirSync, writeFileSync, appendFileSync } from "node:fs";
import { setTimeout as delay } from "node:timers/promises";
import { capture, meetings } from "../node/dist/index.js";

const pid = Number(process.env.TEST_STRESS_PID);
const seconds = Number(process.env.MREC_STRESS_SECONDS ?? 120);
const cycles = Number(process.env.MREC_STRESS_CYCLES ?? 50);
assert(Number.isInteger(pid) && pid > 1);
assert(seconds >= 10 && seconds <= 3600);
function resources() {
  return {
    fds: readdirSync("/proc/self/fd").length,
    threads: readdirSync("/proc/self/task").length,
    rss: Number(readFileSync("/proc/self/status", "utf8").match(/VmRSS:\s+(\d+)/)[1]) * 1024,
  };
}
function collect(track, frequency, name) {
  const stats = {
    frames: 0,
    packets: 0,
    first: null,
    end: null,
    maxGapMs: 0,
    powers: [],
    energies: [],
  };
  let count = 0,
    re = 0,
    im = 0,
    energy = 0;
  track.on("data", (chunk) => {
    if (process.env.MREC_STRESS_PCM)
      appendFileSync(`${process.env.MREC_STRESS_PCM}-${name}.f32`, chunk.pcm);
    assert.equal(chunk.droppedSamples, 0);
    assert.equal(chunk.sampleRate, 48000);
    assert.equal(chunk.channels, 1);
    assert.equal(chunk.pcm.length, chunk.frameCount * 4);
    assert(chunk.hostTimeNs <= process.hrtime.bigint() + 20_000_000n);
    if (stats.end !== null) {
      assert(chunk.hostTimeNs >= stats.end, "overlapping or regressing PCM");
      stats.maxGapMs = Math.max(stats.maxGapMs, Number(chunk.hostTimeNs - stats.end) / 1e6);
    }
    stats.first ??= chunk.hostTimeNs;
    stats.end = chunk.hostTimeNs + (BigInt(chunk.frameCount) * 1_000_000_000n) / 48000n;
    stats.frames += chunk.frameCount;
    stats.packets++;
    for (let i = 0; i < chunk.frameCount; i++) {
      const value = chunk.pcm.readFloatLE(i * 4);
      assert(Number.isFinite(value));
      const angle = (2 * Math.PI * frequency * count) / 48000;
      re += value * Math.cos(angle);
      im += value * Math.sin(angle);
      energy += value * value;
      // Match the Windows E2E's 100 ms spectral blocks. Device resampling can
      // change phase within a second without losing PCM or the test tone.
      if (++count === 4800) {
        stats.powers.push((2 * (re * re + im * im)) / count / Math.max(energy, 1e-12));
        stats.energies.push(energy / count);
        count = re = im = energy = 0;
      }
    }
  });
  return stats;
}

const target = { type: "process", pid };
async function shortSession() {
  const session = await capture.start(target, { microphone: "default" });
  let frames = 0;
  session.systemAudio.on("data", (chunk) => (frames += chunk.frameCount));
  session.microphone.on("data", () => {});
  await delay(350);
  const start = performance.now();
  await session.stopRecording();
  assert(performance.now() - start < 2000, "stop exceeded two seconds");
  assert(frames > 0, "restart produced no audio");
  return frames;
}
for (let i = 0; i < 3; i++) await shortSession();
await delay(200);
const baseline = resources();
meetings.watch();
for (let i = 0; i < cycles; i++) {
  await shortSession();
  await meetings.scan();
}
meetings.unwatch();
await delay(200);
const afterCycles = resources();
assert(afterCycles.fds <= baseline.fds + 4, "file descriptors leaked across capture restarts");
assert(afterCycles.threads <= baseline.threads + 2, "threads leaked across capture restarts");
console.log(`${cycles} capture restarts with concurrent meeting watcher passed`, {
  baseline,
  afterCycles,
});

const session = await capture.start(target, { microphone: "default", mix: { sampleRate: 16000 } });
const system = collect(session.systemAudio, 440, "system");
const microphone = collect(session.microphone, 660, "microphone");
let mixedBytes = 0;
session.mixedAudio.on("data", (data) => (mixedBytes += data.length));
const start = performance.now();
const cpuStart = process.cpuUsage();
try {
  for (let elapsed = 0; elapsed < seconds; elapsed++) {
    await delay(1000);
    await meetings.scan();
    if ((elapsed + 1) % 30 === 0) console.log("soak seconds", elapsed + 1, resources());
  }
} finally {
  await session.stopRecording();
}
const elapsedSeconds = (performance.now() - start) / 1000;
const final = resources();
const report = {
  cycles,
  seconds,
  elapsedSeconds,
  cpu: process.cpuUsage(cpuStart),
  baseline,
  afterCycles,
  final,
  mixedSeconds: mixedBytes / 32000,
  spectralBlockFrames: 4800,
  system,
  microphone,
};
const output = process.env.MREC_STRESS_OUTPUT ?? "native/build/linux/artifacts/stress.json";
mkdirSync(output.slice(0, output.lastIndexOf("/")), { recursive: true });
writeFileSync(
  output,
  JSON.stringify(report, (_, value) => (typeof value === "bigint" ? String(value) : value), 2),
);
for (const [name, stats] of Object.entries({ system, microphone })) {
  assert(stats.frames / 48000 > elapsedSeconds - 1.5, `${name} lost sustained PCM`);
  assert(stats.frames / 48000 < elapsedSeconds + 0.5, `${name} delivered excess PCM`);
  assert.equal(stats.maxGapMs, 0, `${name} timeline has gaps`);
  const stable = stats.powers.slice(10, -10);
  assert(stable.length > (seconds - 5) * 10);
  assert(
    stats.energies.slice(10, -10).every((energy) => energy > 1e-8),
    `${name} has silent blocks`,
  );
  assert(
    stable.reduce((total, power) => total + power, 0) / stable.length > 0.7,
    `${name} tone disappeared during sustained capture`,
  );
}
assert(Math.abs(mixedBytes / 32000 - system.frames / 48000) < 0.5, "mixed duration drifted");
assert(final.fds <= baseline.fds + 4, "file descriptors leaked during soak");
assert(final.rss < baseline.rss + 64 * 1024 * 1024, "unbounded capture memory growth");
console.log(
  "sustained PCM, timing, tone integrity, mixed output, and bounded resources passed",
  output,
);
