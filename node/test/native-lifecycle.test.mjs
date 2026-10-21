import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import test, { before, after, beforeEach, afterEach } from "node:test";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const require = createRequire(import.meta.url);
const root = fileURLToPath(new URL("../../", import.meta.url));
// Windows locks loaded addons until exit; leave this fixture in build output.
const directory = join(root, "node/build/test-native");
let api;
let native;
const recordings = new Set();

async function start(target = { type: "system" }, options) {
  const recording = await api.capture.start(target, options);
  recordings.add(recording);
  return recording;
}

async function until(check) {
  for (let attempt = 0; attempt < 500; attempt++) {
    if (check()) return;
    await delay(5);
  }
  assert.fail("timed out waiting for native delivery");
}
before(async () => {
  await mkdir(directory, { recursive: true });
  let binding = await readFile(join(root, "node/src/native/binding.cc"), "utf8");
  assert.ok(binding.includes("Napi::Object Init(Napi::Env env, Napi::Object exports) {"));
  binding = binding.replace(
    "Napi::Object Init(Napi::Env env, Napi::Object exports) {",
    "void RegisterTestBackend(Napi::Env, Napi::Object);\nNapi::Object Init(Napi::Env env, Napi::Object exports) {\n  RegisterTestBackend(env, exports);",
  );
  await writeFile(join(directory, "binding.cc"), binding);
  await copyFile(
    join(root, "node/test/fixtures/native-backend.cc"),
    join(directory, "native-backend.cc"),
  );
  await writeFile(
    join(directory, "binding.gyp"),
    JSON.stringify({
      targets: [
        {
          target_name: "meeting_record_test",
          sources: ["binding.cc", "native-backend.cc"],
          include_dirs: [dirname(require.resolve("node-addon-api")), join(root, "native/include")],
          defines: ["NAPI_DISABLE_CPP_EXCEPTIONS", "NAPI_VERSION=8"],
          cflags_cc: ["-std=c++17"],
          xcode_settings: {
            CLANG_CXX_LANGUAGE_STANDARD: "c++17",
            MACOSX_DEPLOYMENT_TARGET: "14.2",
          },
          msvs_settings: { VCCLCompilerTool: { AdditionalOptions: ["/std:c++17"] } },
        },
      ],
    }),
  );
  await promisify(execFile)(process.execPath, [
    require.resolve("node-gyp/bin/node-gyp.js"),
    "rebuild",
    "--directory",
    directory,
  ]);
  native = globalThis.meetingRecordTestNative = require(
    join(directory, "build/Release/meeting_record_test.node"),
  );
  const entry = new URL("../dist/index.js", import.meta.url);
  let source = await readFile(entry, "utf8");
  // Inject the test backend into the public wrapper.
  assert.ok(source.includes("let cachedNative;"));
  source = source.replace(
    "let cachedNative;",
    "let cachedNative = globalThis.meetingRecordTestNative;",
  );
  source = source.replaceAll(
    '"./mixed-audio.js"',
    JSON.stringify(new URL("./mixed-audio.js", entry).href),
  );
  api = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
});
beforeEach(() => {
  native.testConfigure({});
});
afterEach(async () => {
  for (const recording of recordings) await recording.stopRecording();
  recordings.clear();
});
after(() => {
  delete globalThis.meetingRecordTestNative;
});

test("domain constants configure capture and match native permission and track state", async () => {
  for (const permission of Object.values(api.Permission)) {
    assert.equal(api.permissions.status(permission), api.PermissionStatus.Granted);
  }
  const recording = await start({ type: "system" }, { microphone: api.MicrophoneSource.Auto });
  try {
    assert.equal(recording.state, api.CaptureState.Recording);
    assert.equal(recording.systemAudio.availability, api.TrackAvailability.Live);
    assert.ok(recording.microphone);
    recording.pauseRecording();
    assert.equal(recording.state, api.CaptureState.Paused);
    recording.resumeRecording();
    assert.equal(recording.state, api.CaptureState.Recording);
  } finally {
    await recording.stopRecording();
  }
  assert.equal(recording.state, api.CaptureState.Stopped);
});

test("standard async iteration retains packet metadata and the native stop tail", async () => {
  const recording = await start({ type: "system" });
  assert.equal(recording.microphone, undefined);
  // Fill the stream to test backpressure.
  await until(() => recording.systemAudio.readableLength === 32);
  assert.equal(recording.systemAudio.readableLength, 32);
  let eventLoopRan = false;
  const stopping = recording.stopRecording();
  assert.equal(api.capture.running, true);
  await assert.rejects(api.capture.start({ type: "system" }), api.AlreadyRunningError);
  assert.equal(recording.stopRecording(), stopping);
  setTimeout(() => {
    eventLoopRan = true;
  }, 10);
  await stopping;
  assert.equal(eventLoopRan, true);
  const packets = [];
  for await (const packet of recording.systemAudio) packets.push(packet);
  assert.equal(packets.length, 42);
  for (const [index, packet] of packets.entries()) {
    assert.equal(packet.pcm.length, 8);
    assert.equal(packet.frameCount, 2);
    assert.equal(packet.sampleRate, 48000);
    assert.equal(packet.channels, 1);
    assert.equal(packet.hostTimeNs, 1_000_000_000n + BigInt(index) * 10_000_000n);
  }
  assert.equal(recording.state, "stopped");
  assert.equal(api.capture.running, false);
});

test("stopping a paused recording excludes new native tail callbacks", async () => {
  const recording = await start({ type: "system" });
  recording.pauseRecording();
  await recording.stopRecording();
  for await (const packet of recording.systemAudio) assert.ok(packet.hostTimeNs < 1_400_000_000n);
});

test("track events set availability and skip unknown native event types", async () => {
  native.testConfigure({ initialEventType: 3 });
  const recording = await start({ type: "process", pid: 1 });
  try {
    await until(() => recording.systemAudio.availability === "degraded");
    assert.equal(recording.systemAudio.availability, "degraded");
    assert.equal(recording.systemAudio.interrupted, false);
    native.testEvent({ track: "systemAudio", type: 99 });
  } finally {
    // A live capture would keep the test process running.
    await recording.stopRecording();
  }
  for await (const packet of recording.systemAudio) assert.equal(packet.channels, 1);
  assert.equal(recording.systemAudio.availability, "degraded");
});

test("enumeration failures stay distinguishable from an empty source list", async () => {
  await assert.rejects(api.meetings.scan(), api.InternalError);
  assert.throws(() => api.meetings.audioProcesses(), api.InternalError);
  assert.throws(() => api.meetings.audioApps(), api.InternalError);
});

for (const mono of [true, false]) {
  test(`${mono ? "mono" : "stereo"} system packets preserve a separate microphone format`, async () => {
    native.testConfigure({ initialPackets: 0, tailPackets: 0 });
    const recording = await start(
      { type: "process", pid: 123 },
      { mono, microphone: api.MicrophoneSource.Auto },
    );
    const system = [];
    const microphone = [];
    recording.systemAudio.on("data", (packet) => system.push(packet));
    recording.microphone.on("data", (packet) => microphone.push(packet));
    assert.equal(recording.systemAudio.channels, mono ? 1 : 2);
    assert.equal(recording.microphone.channels, 2);
    assert.equal(recording.microphone.sampleRate, 44100);
    assert.equal(recording.microphone.deviceName, "Fixture microphone");
    assert.deepEqual(native.testStats(), {
      starts: 1,
      stops: 0,
      microphoneSource: 2,
      pid: 123,
      systemWide: false,
    });
    native.testAudio({ index: 10 });
    native.testAudio({ track: "microphone", index: 10 });
    await until(() => system.length === 1 && microphone.length === 1);
    for (const [packet, channels, rate] of [
      [system[0], mono ? 1 : 2, 48000],
      [microphone[0], 2, 44100],
    ]) {
      assert.equal(packet.pcm.length, packet.frameCount * channels * 4);
      assert.equal(packet.frameCount, 2);
      assert.equal(packet.channels, channels);
      assert.equal(packet.sampleRate, rate);
      assert.equal(packet.hostTimeNs, 1_100_000_000n);
      assert.equal(packet.droppedSamples, 0);
      assert.equal(packet.pcm.readFloatLE(0), 0.25);
      assert.equal(packet.pcm.readFloatLE(4), -0.25);
    }
  });
}

test("silent packets keep both tracks live and preserve their metadata", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  const packets = [];
  for (const [name, track] of [
    ["systemAudio", recording.systemAudio],
    ["microphone", recording.microphone],
  ]) {
    track.on("data", (packet) => packets.push(packet));
    native.testAudio({ track: name, silent: true, index: 12 });
  }
  await until(() => packets.length === 2);
  for (const packet of packets) {
    assert.ok(packet.pcm.equals(Buffer.alloc(packet.frameCount * packet.channels * 4)));
    assert.equal(packet.hostTimeNs, 1_120_000_000n);
    assert.equal(packet.droppedSamples, 0);
  }
  assert.equal(recording.systemAudio.availability, api.TrackAvailability.Live);
  assert.equal(recording.microphone.availability, api.TrackAvailability.Live);
  assert.equal(recording.state, api.CaptureState.Recording);
  assert.equal(native.testStats().stops, 0);
});

test("track events retain order and update state before notifying consumers", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  const events = [];
  for (const [name, track] of [
    ["systemAudio", recording.systemAudio],
    ["microphone", recording.microphone],
  ]) {
    for (const type of ["degraded", "interrupted", "resumed", "devicechange"]) {
      track.on(type, (...args) => {
        events.push([name, type, track.availability, track.deviceName, ...args]);
      });
    }
  }
  native.testEvent({ track: "systemAudio", type: 3, reason: 5, message: "one source lost" });
  native.testEvent({ track: "systemAudio", type: 0, reason: 1, message: "all sources lost" });
  native.testEvent({ track: "microphone", type: 0, reason: 2, message: "input changed" });
  native.testEvent({ track: "microphone", type: 2, deviceName: "Replacement microphone" });
  native.testEvent({ track: "microphone", type: 1 });
  native.testEvent({ track: "systemAudio", type: 3, reason: 5, message: "one source recovered" });
  native.testEvent({ track: "systemAudio", type: 1 });
  await until(() => events.length === 7);
  assert.deepEqual(events, [
    ["systemAudio", "degraded", "degraded", undefined, "service-reset", "one source lost"],
    ["systemAudio", "interrupted", "interrupted", undefined, "device-removed", "all sources lost"],
    [
      "microphone",
      "interrupted",
      "interrupted",
      "Fixture microphone",
      "device-changed",
      "input changed",
    ],
    [
      "microphone",
      "devicechange",
      "interrupted",
      "Replacement microphone",
      "Replacement microphone",
    ],
    ["microphone", "resumed", "live", "Replacement microphone"],
    ["systemAudio", "degraded", "degraded", undefined, "service-reset", "one source recovered"],
    ["systemAudio", "resumed", "live", undefined],
  ]);
  assert.equal(recording.state, api.CaptureState.Recording);
  assert.equal(recording.systemAudio.interrupted, false);
  assert.equal(native.testStats().microphoneSource, 1);
});

test("unknown native tracks and event types are ignored, and unknown reasons are retained", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  const events = [];
  for (const [name, track] of [
    ["systemAudio", recording.systemAudio],
    ["microphone", recording.microphone],
  ]) {
    for (const type of ["interrupted", "resumed", "degraded", "devicechange"]) {
      track.on(type, (...args) => events.push([name, type, ...args]));
    }
  }
  native.testEvent({ track: "microphone", type: 99, deviceName: "unused" });
  native.testEvent({ track: 99, type: 0, reason: 1 });
  native.testEvent({ track: "microphone", type: 0, reason: 99, message: "unknown reason" });
  await until(() => events.length > 0);
  assert.deepEqual(events, [["microphone", "interrupted", "unknown", "unknown reason"]]);
  assert.equal(recording.systemAudio.availability, api.TrackAvailability.Live);
  assert.equal(recording.microphone.deviceName, "Fixture microphone");
  assert.equal(recording.microphone.availability, api.TrackAvailability.Interrupted);
  assert.equal(recording.state, api.CaptureState.Recording);
});

test("format changes precede their packet and leave the other track unchanged", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  const order = [];
  recording.systemAudio.on("format", (format) => {
    assert.equal(recording.systemAudio.sampleRate, format.sampleRate);
    assert.equal(recording.systemAudio.channels, format.channels);
    order.push(["format", format]);
  });
  recording.systemAudio.on("data", (packet) => {
    order.push(["data", packet.sampleRate, packet.channels, packet.pcm.length]);
  });
  native.testAudio({ index: 1, sampleRate: 32000, channels: 2, frameCount: 3 });
  await until(() => order.length === 2);
  native.testAudio({ index: 2, sampleRate: 32000, channels: 2, frameCount: 3 });
  await until(() => order.length === 3);
  assert.deepEqual(order, [
    ["format", { sampleRate: 32000, channels: 2 }],
    ["data", 32000, 2, 24],
    ["data", 32000, 2, 24],
  ]);
  assert.equal(recording.microphone.sampleRate, 44100);
  assert.equal(recording.microphone.channels, 2);
});

test("app exit interrupts system audio while the microphone and session continue", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(
    { type: "process", pid: 123 },
    { microphone: api.MicrophoneSource.Default },
  );
  const microphone = [];
  const interrupted = [];
  let systemEnded = false;
  let microphoneEnded = false;
  recording.systemAudio.on("data", () => {});
  recording.systemAudio.on("end", () => {
    systemEnded = true;
  });
  recording.microphone.on("data", (packet) => microphone.push(packet));
  recording.microphone.on("end", () => {
    microphoneEnded = true;
  });
  recording.systemAudio.on("interrupted", (...args) => interrupted.push(args));
  native.testEvent({ track: "systemAudio", type: 0, reason: 4, message: "target exited" });
  await until(() => interrupted.length === 1);
  native.testAudio({ track: "microphone", index: 20 });
  await until(() => microphone.length === 1);
  assert.deepEqual(interrupted, [["app-exited", "target exited"]]);
  assert.equal(recording.systemAudio.availability, api.TrackAvailability.Interrupted);
  assert.equal(recording.systemAudio.interrupted, true);
  assert.equal(recording.microphone.availability, api.TrackAvailability.Live);
  assert.equal(recording.state, api.CaptureState.Recording);
  assert.equal(api.capture.running, true);
  assert.equal(systemEnded, false);
  assert.equal(microphoneEnded, false);
  await recording.stopRecording();
  await until(() => systemEnded && microphoneEnded);
  assert.equal(recording.systemAudio.availability, api.TrackAvailability.Interrupted);
});

test("pause discards incoming packets and resume keeps their original timestamps", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  const packets = [];
  recording.systemAudio.on("data", (packet) => packets.push(packet));
  recording.pauseRecording();
  recording.pauseRecording();
  native.testAudio({ index: 1 });
  native.testAudio({ track: "microphone", index: 1 });
  recording.resumeRecording();
  recording.resumeRecording();
  native.testAudio({ index: 30 });
  await until(() => packets.length === 1);
  assert.equal(packets[0].hostTimeNs, 1_300_000_000n);
  assert.equal(recording.microphone.readableLength, 0);
  await recording.stopRecording();
  recording.pauseRecording();
  recording.resumeRecording();
  assert.equal(recording.state, api.CaptureState.Stopped);
});

test("aborting a pending start cleans up before allowing another capture", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0, startDelayMs: 75 });
  const controller = new AbortController();
  const reason = new Error("cancel this start");
  const starting = api.capture.start({ type: "system" }, { signal: controller.signal });
  assert.equal(api.capture.running, true);
  await assert.rejects(api.capture.start({ type: "system" }), api.AlreadyRunningError);
  controller.abort(reason);
  await assert.rejects(starting, (error) => error === reason);
  assert.equal(api.capture.running, false);
  assert.equal(native.testStats().stops, 1);
  const recording = await start();
  assert.equal(recording.state, api.CaptureState.Recording);
});

test("aborting a running capture drains both tracks and stops only once", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 2 });
  const controller = new AbortController();
  const recording = await start(undefined, {
    microphone: api.MicrophoneSource.Default,
    signal: controller.signal,
  });
  controller.abort();
  const stopping = recording.stopRecording();
  assert.equal(recording.stopRecording(), stopping);
  recording.pauseRecording();
  recording.resumeRecording();
  await stopping;
  for (const track of [recording.systemAudio, recording.microphone]) {
    const packets = [];
    for await (const packet of track) packets.push(packet);
    assert.equal(packets.length, 2);
  }
  assert.equal(native.testStats().stops, 1);
  assert.equal(api.capture.running, false);
});

for (const [field, status, ErrorType, expectedStops] of [
  ["startStatus", -2, "PermissionError", 0],
  ["formatStatus", -7, "CaptureError", 1],
  ["microphoneFormatStatus", -7, "CaptureError", 1],
]) {
  test(`${field} failure releases the singleton and maps the native error`, async () => {
    native.testConfigure({ initialPackets: 40, tailPackets: 0, [field]: status });
    await assert.rejects(
      api.capture.start({ type: "system" }, { microphone: api.MicrophoneSource.Default }),
      (error) => {
        assert.ok(error instanceof api[ErrorType]);
        assert.equal(error.code, status);
        assert.match(error.message, /fixture failure/);
        return true;
      },
    );
    assert.equal(api.capture.running, false);
    assert.equal(native.testStats().stops, expectedStops);
    native.testConfigure({ initialPackets: 0, tailPackets: 0 });
    const recording = await start();
    const packets = [];
    recording.systemAudio.on("data", (packet) => packets.push(packet));
    native.testAudio({ index: 100 });
    await until(() => packets.length === 1);
    assert.equal(packets[0].hostTimeNs, 2_000_000_000n);
  });
}

test("an interruption during initialization remains visible after start", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0, initialEventType: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  await until(() => recording.systemAudio.interrupted);
  assert.equal(recording.microphone.availability, api.TrackAvailability.Live);
  assert.equal(recording.state, api.CaptureState.Recording);
  assert.equal(api.capture.running, true);
});

test("startup source events reach listeners attached after awaiting start, in order", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const captureStart = native.captureStart;
  native.captureStart = (options, system, microphone, event) => {
    event({ track: "systemAudio", type: "interrupted", reason: "service-reset", message: "lost" });
    event({ track: "microphone", type: "devicechange", deviceName: "Replacement microphone" });
    event({ track: "systemAudio", type: "resumed" });
    return captureStart(options, system, microphone, event);
  };
  let recording;
  try {
    recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  } finally {
    native.captureStart = captureStart;
  }
  const events = [];
  recording.systemAudio.on("interrupted", (reason, message) => {
    events.push(["interrupted", reason, message, recording.systemAudio.availability]);
  });
  recording.microphone.on("devicechange", (name) => {
    events.push(["devicechange", name, recording.microphone.deviceName]);
  });
  recording.systemAudio.on("resumed", () => {
    events.push(["resumed", recording.systemAudio.availability]);
  });
  native.testEvent({ track: "systemAudio", type: 3, reason: 5, message: "later" });
  recording.systemAudio.on("degraded", (reason, message) => {
    events.push(["degraded", reason, message, recording.systemAudio.availability]);
  });
  await until(() => events.length === 4);
  assert.deepEqual(events, [
    ["interrupted", "service-reset", "lost", "interrupted"],
    ["devicechange", "Replacement microphone", "Replacement microphone"],
    ["resumed", "live"],
    ["degraded", "service-reset", "later", "degraded"],
  ]);
});

test("native overflow reports interleaved sample loss on the affected track", async () => {
  native.testConfigure({ initialPackets: 0, tailPackets: 0 });
  const recording = await start(undefined, { microphone: api.MicrophoneSource.Default });
  const dropped = [];
  const packets = [];
  recording.systemAudio.on("drop", (count) => dropped.push(count));
  recording.systemAudio.on("data", (packet) => packets.push(packet));
  // A packet larger than the native ring cannot fit, even with a fast consumer.
  native.testAudio({ index: 1, channels: 2, frameCount: 1_048_577 });
  native.testAudio({ index: 2 });
  await until(() => packets.length === 1);
  assert.deepEqual(dropped, [2_097_154]);
  assert.equal(packets[0].droppedSamples, 2_097_154);
  assert.equal(recording.systemAudio.droppedSamples, 2_097_154);
  assert.equal(recording.microphone.droppedSamples, 0);
  assert.equal(recording.systemAudio.availability, api.TrackAvailability.Live);
});
