import assert from "node:assert/strict";
import test from "node:test";

import * as MeetingRecord from "../dist/index.js";

const nativeSupported =
  ["darwin", "win32"].includes(process.platform) && ["arm64", "x64"].includes(process.arch);

function assertCaptureInactive() {
  if (nativeSupported) {
    assert.equal(MeetingRecord.capture.running, false);
  } else {
    assert.throws(() => MeetingRecord.capture.running, MeetingRecord.UnsupportedOsError);
  }
}

test("invalid public inputs use TypeError without starting capture", async () => {
  assert.throws(() => MeetingRecord.permissions.status("camera"), TypeError);
  for (const pid of [0, -1, 1.5, 2 ** 31, 2 ** 32 + 1]) {
    await assert.rejects(MeetingRecord.capture.start({ type: "process", pid }), TypeError);
  }
  await assert.rejects(
    MeetingRecord.capture.start({ type: "system" }, { microphone: "built-in" }),
    TypeError,
  );
  await assert.rejects(
    MeetingRecord.capture.start({ type: "system" }, { mix: { sampleRate: 0 } }),
    TypeError,
  );
  assertCaptureInactive();
});

test("cancel capture and permission requests before native work starts", async () => {
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    MeetingRecord.capture.start({ type: "system" }, { signal: controller.signal }),
    { name: "AbortError" },
  );
  await assert.rejects(
    MeetingRecord.permissions.request("microphone", { signal: controller.signal }),
    { name: "AbortError" },
  );
  assertCaptureInactive();
});
