import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import test, { before, after } from "node:test";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const require = createRequire(import.meta.url);
const root = fileURLToPath(new URL("../../", import.meta.url));
// Loaded addons stay locked until process exit on Windows. Keep the fixture
// alongside other build output so ordinary build cleanup can remove it later.
const directory = join(root, "node/build/test-native");
let api;
before(async () => {
  await mkdir(directory, { recursive: true });
  await copyFile(join(root, "node/src/native/binding.cc"), join(directory, "binding.cc"));
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
  globalThis.meetingRecordTestNative = require(
    join(directory, "build/Release/meeting_record_test.node"),
  );
  const entry = new URL("../dist/index.js", import.meta.url);
  let source = await readFile(entry, "utf8");
  // Substitute only the private native loader; exercise the actual public wrapper.
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
after(() => {
  delete globalThis.meetingRecordTestNative;
});

test("standard async iteration retains packet metadata and the native stop tail", async () => {
  const recording = await api.capture.start({ type: "system" });
  // Let the unread object stream apply backpressure while the ring retains its tail.
  for (let attempt = 0; recording.systemAudio.readableLength < 32 && attempt < 100; attempt++)
    await delay(5);
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
  const recording = await api.capture.start({ type: "system" });
  recording.pauseRecording();
  await recording.stopRecording();
  for await (const packet of recording.systemAudio) assert.ok(packet.hostTimeNs < 1_400_000_000n);
});

test("enumeration failures stay distinguishable from an empty source list", async () => {
  await assert.rejects(api.meetings.scan(), api.InternalError);
  assert.throws(() => api.meetings.audioProcesses(), api.InternalError);
  assert.throws(() => api.meetings.audioApps(), api.InternalError);
});
