// Instrument a real browser call. UI and sign-in stay in the normal browser.
import { createWriteStream } from "node:fs";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { createInterface } from "node:readline";
import { finished } from "node:stream/promises";
import { capture, meetings, permissions, Permission } from "../node/dist/index.js";

const pid = Number(process.argv[2]);
if (!Number.isInteger(pid) || pid < 1)
  throw new Error("usage: node e2e/linux-live.mjs PID [output-directory]");
process.kill(pid, 0);
const output = resolve(process.argv[3] ?? `native/build/linux/artifacts/live-${Date.now()}`);
await mkdir(output, { recursive: true, mode: 0o700 });
const events = createWriteStream(`${output}/events.jsonl`, { flags: "wx", mode: 0o600 });
const json = (value) =>
  JSON.stringify(value, (_, item) => (typeof item === "bigint" ? item.toString() : item));
let logSequence = 0;
function log(event, detail = {}) {
  const line = json({
    sequence: ++logSequence,
    monotonicNs: process.hrtime.bigint(),
    at: new Date().toISOString(),
    event,
    ...detail,
  });
  events.write(`${line}\n`);
  if (event !== "scan") console.log(line);
}
let recording;
let tracks = [];
let mixed = [];
let label;
let sequence = 0;
let closing = false;
let scanning = false;
const recordings = [];
const metrics = () =>
  tracks.map(({ name, stats, sumSquares, samples }) => ({
    name,
    ...stats,
    rms: Math.sqrt(sumSquares / Math.max(1, samples)),
  }));
async function snapshot(mark) {
  log("checkpoint", {
    mark,
    meetings: await meetings.scan(),
    apps: meetings.audioApps(),
    tracks: metrics(),
  });
}
async function start(recordName) {
  if (recording) throw new Error("already recording");
  label = `${++sequence}-${(recordName || "call").replace(/[^a-zA-Z0-9_-]/g, "_")}`;
  tracks = [];
  mixed = [];
  recording = await capture.start(
    { type: "process", pid },
    { microphone: "auto", mix: { sampleRate: 16000 } },
  );
  for (const [name, track] of [
    ["system", recording.systemAudio],
    ["microphone", recording.microphone],
  ]) {
    const item = {
      name,
      stats: {
        packets: 0,
        frames: 0,
        seconds: 0,
        peak: 0,
        droppedSamples: 0,
        timestampRegressions: 0,
        malformedPackets: 0,
        nonfinite: 0,
        maxGapMs: 0,
        maxAgeMs: 0,
        maxFutureMs: 0,
        formats: [],
      },
      samples: 0,
      sumSquares: 0,
      last: undefined,
      expected: undefined,
      streams: [],
      stream: undefined,
    };
    tracks.push(item);
    for (const event of [
      "format",
      "interrupted",
      "resumed",
      "degraded",
      "devicechange",
      "drop",
      "error",
    ])
      track.on(event, (...args) =>
        log(`track-${event}`, {
          track: name,
          args: args.map((a) => (a instanceof Error ? a.message : a)),
        }),
      );
    track.on("data", (chunk) => {
      const { stats } = item;
      const ageMs = Number(process.hrtime.bigint() - chunk.hostTimeNs) / 1e6;
      stats.maxAgeMs = Math.max(stats.maxAgeMs, ageMs);
      stats.maxFutureMs = Math.max(stats.maxFutureMs, -ageMs);
      if (chunk.pcm.length !== chunk.frameCount * chunk.channels * 4) stats.malformedPackets++;
      if (item.last !== undefined && chunk.hostTimeNs < item.last) stats.timestampRegressions++;
      if (item.expected !== undefined)
        stats.maxGapMs = Math.max(stats.maxGapMs, Number(chunk.hostTimeNs - item.expected) / 1e6);
      item.last = chunk.hostTimeNs;
      item.expected =
        chunk.hostTimeNs + BigInt(Math.round((chunk.frameCount * 1e9) / chunk.sampleRate));
      const format = `${chunk.sampleRate}-${chunk.channels}`;
      if (stats.formats.at(-1)?.format !== format) {
        item.stream?.end();
        const file = `${label}-${name}-${stats.formats.length + 1}.f32`;
        item.stream = createWriteStream(`${output}/${file}`, { flags: "wx", mode: 0o600 });
        item.streams.push(item.stream);
        stats.formats.push({
          format,
          file,
          sampleRate: chunk.sampleRate,
          channels: chunk.channels,
        });
      }
      item.stream.write(chunk.pcm);
      stats.packets++;
      stats.frames += chunk.frameCount;
      stats.seconds += chunk.frameCount / chunk.sampleRate;
      stats.droppedSamples += chunk.droppedSamples;
      for (let offset = 0; offset + 4 <= chunk.pcm.length; offset += 4) {
        const sample = chunk.pcm.readFloatLE(offset);
        if (!Number.isFinite(sample)) {
          stats.nonfinite++;
          continue;
        }
        stats.peak = Math.max(stats.peak, Math.abs(sample));
        item.sumSquares += sample * sample;
        item.samples++;
      }
    });
  }
  recording.mixedAudio.on("data", (chunk) => mixed.push(Buffer.from(chunk)));
  recording.mixedAudio.on("error", (error) => log("mixed-error", { message: error.message }));
  log("recording-started", { label, microphone: recording.microphone.deviceName });
}
async function stop() {
  if (!recording) return;
  await recording.stopRecording();
  recording = undefined;
  for (const track of tracks) track.stream?.end();
  await Promise.all(tracks.flatMap((track) => track.streams.map((stream) => finished(stream))));
  const pcm = Buffer.concat(mixed);
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcm.length, 4);
  header.write("WAVEfmt ", 8);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(16000, 24);
  header.writeUInt32LE(32000, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write("data", 36);
  header.writeUInt32LE(pcm.length, 40);
  await writeFile(`${output}/${label}-mixed.wav`, Buffer.concat([header, pcm]), { mode: 0o600 });
  const result = { label, tracks: metrics(), mixedBytes: pcm.length };
  recordings.push(result);
  log("recording-stopped", result);
  await writeFile(`${output}/recordings.json`, json(recordings), { mode: 0o600 });
}
async function finish() {
  if (closing) return;
  closing = true;
  clearInterval(poll);
  clearTimeout(timeout);
  meetings.unwatch();
  await stop();
  log("finished", { output });
  input.close();
  events.end();
  await finished(events);
}
for (const event of ["started", "updated", "ended"])
  meetings.on(event, (meeting) => log(`meeting-${event}`, { meeting }));
meetings.watch();
const poll = setInterval(async () => {
  if (closing || scanning) return;
  scanning = true;
  try {
    const found = await meetings.scan();
    if (!closing) log("scan", { meetings: found });
  } catch (error) {
    if (!closing) log("scan-error", { message: error.message });
  } finally {
    scanning = false;
  }
}, 2000);
log("ready", {
  pid,
  output,
  permissions: Object.fromEntries(Object.values(Permission).map((p) => [p, permissions.status(p)])),
});
await snapshot("initial");
const input = createInterface({ input: process.stdin });
let commands = Promise.resolve();
input.on("line", (line) => {
  commands = commands
    .then(async () => {
      if (closing) return;
      const [command, ...words] = line.trim().split(/\s+/);
      const detail = words.join(" ");
      if (command === "start") await start(detail);
      else if (command === "stop") await stop();
      else if (command === "mark" || command === "scan") await snapshot(detail || command);
      else if (command === "pause" || command === "resume") {
        if (!recording) throw new Error("not recording");
        if (command === "pause") recording.pauseRecording();
        else recording.resumeRecording();
        log(command, { tracks: metrics() });
      } else if (command === "finish") await finish();
      else throw new Error("commands: mark LABEL | start LABEL | pause | resume | stop | finish");
    })
    .catch((error) => log("command-error", { message: error.message }));
});
const timeout = setTimeout(
  () => {
    void finish();
  },
  15 * 60 * 1000,
);
input.once("close", () => {
  void commands.then(() => finish());
});
process.once("SIGINT", () => {
  void finish();
});
process.once("SIGTERM", () => {
  void finish();
});
