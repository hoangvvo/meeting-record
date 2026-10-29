import assert from "node:assert/strict";
import { execFileSync, spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { capture, meetings, permissions, Permission } from "../node/dist/index.js";

const a = Number(process.env.TEST_PID_A);
const b = Number(process.env.TEST_PID_B);
const pactl = (...args) => execFileSync("pactl", args, { encoding: "utf8" }).trim();
async function until(predicate, description, ms = 7000) {
  const deadline = performance.now() + ms;
  while (!(await predicate())) {
    if (performance.now() >= deadline) throw new Error(`timed out: ${description}`);
    await delay(50);
  }
}
function collect(track) {
  const stats = { samples: [], frames: 0, packets: 0, last: 0n, events: [] };
  for (const event of ["interrupted", "resumed", "devicechange", "degraded"])
    track.on(event, (value) => stats.events.push({ event, value }));
  track.on("data", (chunk) => {
    assert.equal(chunk.sampleRate, 48000);
    assert.equal(chunk.pcm.length, chunk.frameCount * chunk.channels * 4);
    assert.equal(chunk.droppedSamples, 0);
    assert(chunk.hostTimeNs >= stats.last, "timestamps regressed");
    assert(
      chunk.hostTimeNs <= process.hrtime.bigint() + 20_000_000n,
      "capture timestamp is in the future",
    );
    stats.last = chunk.hostTimeNs;
    stats.frames += chunk.frameCount;
    stats.packets++;
    for (let i = 0; i < chunk.frameCount; ++i)
      stats.samples.push(chunk.pcm.readFloatLE(i * chunk.channels * 4));
  });
  return stats;
}
// Windowed spectral power tolerates packet boundaries while detecting leakage.
function tonePower(samples, frequency) {
  let power = 0,
    energy = 0,
    windows = 0;
  const length = 4800;
  for (let start = 4800; start + length <= samples.length; start += length) {
    let re = 0,
      im = 0,
      sum = 0;
    for (let i = 0; i < length; i++) {
      const value = samples[start + i];
      re += value * Math.cos((2 * Math.PI * frequency * i) / 48000);
      im += value * Math.sin((2 * Math.PI * frequency * i) / 48000);
      sum += value * value;
    }
    power += (2 * (re * re + im * im)) / length;
    energy += sum;
    windows++;
  }
  assert(windows > 2, "not enough samples for spectral verification");
  return power / Math.max(energy, 1e-12);
}
async function record(target, options = {}, duration = 1500) {
  const session = await capture.start(target, options);
  const system = collect(session.systemAudio);
  const microphone = session.microphone && collect(session.microphone);
  let mixedBytes = 0;
  session.mixedAudio?.on("data", (data) => {
    mixedBytes += data.length;
  });
  await until(
    () => system.frames >= 4800 && (!microphone || microphone.frames >= 4800),
    "audio startup",
  );
  system.frames = 0;
  system.samples = [];
  system.packets = 0;
  if (microphone) {
    microphone.frames = 0;
    microphone.samples = [];
  }
  await delay(duration);
  await session.stopRecording();
  await delay(30);
  assert(
    system.frames > duration * 48 * 0.7,
    `missing system audio: frames=${system.frames} packets=${system.packets}`,
  );
  assert.equal(capture.running, false);
  return { session, system, microphone, mixedBytes };
}

assert.equal(await permissions.status(Permission.SystemAudio), "granted");
const apps = meetings.audioApps();
assert(apps.some((app) => app.pid === a && app.isPlayingAudio && app.isUsingMic));
assert(apps.some((app) => app.pid === b && app.isPlayingAudio));
const detected = await meetings.scan();
assert(
  detected.some(
    (meeting) => meeting.pid === a && meeting.platform === "zoom" && meeting.shouldRecord,
  ),
);
console.log("enumeration and native meeting audio fallback passed");

const first = await record(
  { type: "process", pid: a },
  { mono: false, microphone: "default", mix: { sampleRate: 16000 } },
);
console.log("process metrics", {
  frames: first.system.frames,
  tone: tonePower(first.system.samples, 440),
  leakage: tonePower(first.system.samples, 880),
  mic: tonePower(first.microphone.samples, 660),
});
assert.equal(first.session.systemAudio.channels, 2);
assert(tonePower(first.system.samples, 440) > 0.7, "selected app tone missing");
assert(tonePower(first.system.samples, 880) < 0.01, "other app leaked into process recording");
assert(tonePower(first.microphone.samples, 660) > 0.7, "microphone tone missing");
assert(first.mixedBytes > 16000, "mixed output missing");
console.log("isolated process + stereo + separate microphone + mixed output passed");

const selfTone = spawn(`${process.env.MREC_LINUX_BUILD}/other-tone`, ["mrec_speaker", "1100"], {
  stdio: "ignore",
});
await delay(300);
const global = await record({ type: "system" });
selfTone.kill();
console.log(
  "global spectral power",
  [440, 880, 660, 990].map((frequency) => [frequency, tonePower(global.system.samples, frequency)]),
);
for (const frequency of [440, 880, 660, 990])
  assert(tonePower(global.system.samples, frequency) > 0.08, `global tone ${frequency} missing`);
assert(
  tonePower(global.system.samples, 1100) < 0.01,
  "recorder's own child leaked into global output",
);
console.log("global capture across multiple outputs passed");

const helpers = await capture.start({ type: "process", pid: process.pid });
const helperStats = collect(helpers.systemAudio);
await delay(300);
const helper = spawn(`${process.env.MREC_LINUX_BUILD}/other-tone`, ["mrec_speaker", "1100"], {
  stdio: "ignore",
});
await delay(600);
helperStats.samples = [];
await delay(1200);
helper.kill();
await helpers.stopRecording();
assert(tonePower(helperStats.samples, 1100) > 0.6, "late helper missing");
assert(tonePower(helperStats.samples, 440) < 0.01, "unrelated app leaked into helper capture");
console.log("idle target, late helper membership, and self-output exclusion passed");

const exclusive = await capture.start({ type: "process", pid: b });
exclusive.systemAudio.resume();
await assert.rejects(capture.start({ type: "system" }));
await exclusive.stopRecording();
const aborted = new AbortController();
const aborting = await capture.start({ type: "process", pid: b }, { signal: aborted.signal });
aborting.systemAudio.resume();
aborted.abort();
await until(() => !capture.running, "abort cleanup");
console.log("exclusive session and cancellation cleanup passed");

const pause = await capture.start({ type: "process", pid: a }, { microphone: "auto" });
const pauseSystem = collect(pause.systemAudio);
collect(pause.microphone);
await delay(400);
pause.pauseRecording();
await delay(100);
const pausedFrames = pauseSystem.frames;
await delay(400);
assert.equal(pauseSystem.frames, pausedFrames, "pause delivered audio");
pause.resumeRecording();
await until(() => pauseSystem.frames > pausedFrames + 4800, "resume audio");
await pause.stopRecording();
console.log("pause/resume and automatic microphone selection passed");

const switching = await capture.start({ type: "process", pid: a }, { microphone: "default" });
collect(switching.systemAudio);
const micStats = collect(switching.microphone);
await delay(500);
pactl("set-default-source", "mrec_input2");
await until(
  () => switching.microphone.deviceName === "MrecMicrophone2",
  "default microphone switch",
);
micStats.samples = [];
await delay(1000);
assert(tonePower(micStats.samples, 990) > 0.7, "switched mic audio missing");
const module = pactl("list", "short", "modules")
  .split("\n")
  .find((line) => line.includes("source_name=mrec_input2"))
  .split("\t")[0];
const originalModule = pactl("list", "short", "modules")
  .split("\n")
  .find((line) => line.includes("source_name=mrec_input "))
  .split("\t")[0];
pactl("unload-module", originalModule);
pactl("unload-module", module);
await until(() => switching.microphone.interrupted, "microphone interruption");
pactl(
  "load-module",
  "module-remap-source",
  "master=mrec_mic2_sink.monitor",
  "source_name=mrec_input2",
  "source_properties=device.description=MrecMicrophone2",
);
pactl("set-default-source", "mrec_input2");
await until(() => !switching.microphone.interrupted, "microphone recovery").catch((error) => {
  console.error(
    "microphone recovery state",
    pactl("info"),
    pactl("list", "short", "sources"),
    micStats.events,
  );
  throw error;
});
await switching.stopRecording();
const eventCount = micStats.events.length;
await delay(400);
assert.equal(micStats.events.length, eventCount, "event arrived after stop");
pactl(
  "load-module",
  "module-remap-source",
  "master=mrec_mic_sink.monitor",
  "source_name=mrec_input",
  "source_properties=device.description=MrecMicrophone",
);
pactl("set-default-source", "mrec_input");
console.log("microphone switching, removal, recovery, and stop ordering passed");

// A sink-input move must rebuild its monitor instead of capturing its old output.
const moved = await capture.start({ type: "process", pid: b });
const movedStats = collect(moved.systemAudio);
await delay(500);
const inputs = JSON.parse(pactl("-f", "json", "list", "sink-inputs"));
const input = inputs.find((item) => Number(item.properties["application.process.id"]) === b);
assert(input, "playback stream not found");
pactl("move-sink-input", String(input.index), "mrec_speaker");
movedStats.samples = [];
await delay(1200);
await moved.stopRecording();
assert(tonePower(movedStats.samples, 880) > 0.6, "audio lost after output switch");
assert(tonePower(movedStats.samples, 440) < 0.01, "new output's other app leaked in");
console.log("output migration preserves per-app isolation passed");

const watchEvents = [];
for (const event of ["started", "updated", "ended"])
  meetings.on(event, (meeting) => watchEvents.push({ meeting, event }));
meetings.watch();
await until(
  () => watchEvents.some((item) => item.event === "started" && item.meeting.pid === a),
  "meeting watcher start",
);
const exiting = await capture.start({ type: "process", pid: a }, { microphone: "default" });
const exitedSystem = collect(exiting.systemAudio);
const survivingMic = collect(exiting.microphone);
process.kill(a, "SIGTERM");
await until(
  () =>
    exitedSystem.events.some((item) => item.event === "interrupted" && item.value === "app-exited"),
  "app exit interruption",
);
await delay(150);
const afterExit = exitedSystem.frames;
const beforeMic = survivingMic.frames;
await delay(400);
assert.equal(exitedSystem.frames, afterExit, "system audio continued after app exit");
assert(survivingMic.frames > beforeMic, "microphone stopped with app");
await until(
  () => watchEvents.some((item) => item.event === "ended" && item.meeting.pid === a),
  "meeting watcher end",
).catch(async (error) => {
  console.error("watch events", watchEvents, "scan", await meetings.scan());
  throw error;
});
await exiting.stopRecording();
await delay(30);
assert.equal(exitedSystem.frames, afterExit, "app-exit audio tail was held until session stop");
meetings.unwatch();
console.log("app exit, surviving microphone, and debounced meeting watcher passed");

await assert.rejects(capture.start({ type: "process", pid: 2147483647 }));
assert.equal(capture.running, false);
console.log("invalid process startup releases session passed");
