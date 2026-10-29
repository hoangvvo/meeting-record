import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import { capture, meetings, permissions, Permission } from "../node/dist/index.js";
const pid = Number(process.env.TEST_BROWSER_PID);
async function until(predicate, description, ms = 10000) {
  const deadline = performance.now() + ms;
  while (!(await predicate())) {
    if (performance.now() >= deadline) throw new Error(`timed out: ${description}`);
    await delay(150);
  }
}
let page;
await until(async () => {
  try {
    const pages = await (await fetch("http://127.0.0.1:9223/json/list")).json();
    page = pages.find((item) => item.url.includes("meet.google.com:9443"));
    return page;
  } catch {
    return false;
  }
}, "Chrome startup");
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve) => socket.addEventListener("open", resolve, { once: true }));
let nextId = 1;
async function evaluate(expression) {
  const id = nextId++;
  const response = new Promise((resolve) => {
    const handler = (event) => {
      const message = JSON.parse(event.data);
      if (message.id !== id) return;
      socket.removeEventListener("message", handler);
      resolve(message);
    };
    socket.addEventListener("message", handler);
  });
  socket.send(
    JSON.stringify({
      id,
      method: "Runtime.evaluate",
      params: { expression, awaitPromise: true, returnByValue: true },
    }),
  );
  const result = await response;
  assert(!result.error && !result.result.exceptionDetails, JSON.stringify(result));
  await delay(400);
  return result.result.result.value;
}
assert.equal(permissions.status(Permission.Accessibility), "granted");
await until(
  async () =>
    (await meetings.scan()).some(
      (item) =>
        item.pid === pid && item.platform === "meet" && item.shouldRecord && !item.isUsingMic,
    ),
  "silent joined call through AT-SPI",
);
console.log("AT-SPI detects a silent browser call and ignores controls in an unrelated iframe");
const call = await evaluate("window.startCall()");
console.log("WebRTC call", call);
await until(
  async () =>
    (await meetings.scan()).some(
      (item) => item.pid === pid && item.isUsingMic && item.isPlayingAudio,
    ),
  "browser media activity attribution",
);
const recording = await capture.start(
  { type: "process", pid },
  { microphone: "auto", mix: { sampleRate: 16000 } },
);
const tracks = [[], []];
const frames = [0, 0];
let mixed = 0;
for (const [index, track] of [recording.systemAudio, recording.microphone].entries()) {
  track.on("data", (chunk) => {
    assert.equal(chunk.droppedSamples, 0);
    frames[index] += chunk.frameCount;
    for (let i = 0; i < chunk.frameCount; i++)
      tracks[index].push(chunk.pcm.readFloatLE(i * chunk.channels * 4));
  });
}
recording.mixedAudio.on("data", (data) => {
  mixed += data.length;
});
await until(() => Math.min(...frames) > 96000, "WebRTC system and microphone capture").catch(
  async (error) => {
    console.error(
      "WebRTC capture frames",
      frames,
      "state",
      await evaluate("receiver.connectionState"),
    );
    throw error;
  },
);
await recording.stopRecording();
function power(samples, frequency) {
  // Analyse a contiguous final second, after WebRTC and Pulse startup.
  samples = samples.slice(-48000);
  let re = 0,
    im = 0,
    energy = 0;
  for (let i = 0; i < samples.length; i++) {
    re += samples[i] * Math.cos((2 * Math.PI * frequency * i) / 48000);
    im += samples[i] * Math.sin((2 * Math.PI * frequency * i) / 48000);
    energy += samples[i] ** 2;
  }
  return (2 * (re ** 2 + im ** 2)) / samples.length / Math.max(energy, 1e-12);
}
assert(power(tracks[0], 440) > 0.5, "WebRTC receive audio missing from system track");
assert(power(tracks[0], 660) < 0.01, "microphone source leaked into browser system capture");
assert(power(tracks[1], 660) > 0.7, "selected Linux microphone audio missing");
assert(mixed > 32000, "WebRTC mixed output missing");
console.log(
  "real WebRTC receive audio, Linux microphone, per-browser isolation, and mixed output passed",
);
await evaluate('document.getElementById("controls").innerHTML = "<button>Join now</button>"');
await until(
  async () => !(await meetings.scan()).some((item) => item.pid === pid && item.shouldRecord),
  "pre-join veto",
);
console.log("AT-SPI pre-join suppresses detection");
await evaluate('document.getElementById("controls").innerHTML = "<button>Leave call</button>"');
await until(
  async () => (await meetings.scan()).some((item) => item.pid === pid && item.shouldRecord),
  "rejoin after accessibility tree changes",
);
socket.send(
  JSON.stringify({
    id: nextId++,
    method: "Page.navigate",
    params: { url: "https://unrelated.example:9443/notes" },
  }),
);
await until(async () => {
  try {
    return await evaluate(
      'location.hostname === "unrelated.example" && typeof window.startCall === "function"',
    );
  } catch {
    return false;
  }
}, "unrelated page navigation");
await evaluate("window.startCall()");
await until(
  () =>
    meetings.audioApps().some((item) => item.pid === pid && item.isUsingMic && item.isPlayingAudio),
  "unrelated page media activity",
);
await until(
  async () => !(await meetings.scan()).some((item) => item.pid === pid && item.shouldRecord),
  "unrelated page scoping",
);
console.log("AT-SPI refreshes changed controls and rejects unrelated pages");
socket.close();
