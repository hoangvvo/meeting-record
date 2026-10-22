import assert from "node:assert/strict";
import { once } from "node:events";
import test from "node:test";
import { setTimeout as delay } from "node:timers/promises";
import { MixedAudioImpl } from "../dist/mixed-audio.js";

class Source {
  ready = true;
  consumer;
  setMixConsumer(consumer) {
    this.consumer = consumer;
  }
  setMixReady(ready) {
    this.ready = ready;
  }
  resume() {
    return this;
  }
  send(frames, value, sampleRate, hostTimeNs) {
    const samples = new Float32Array(frames).fill(value);
    const chunk = {
      pcm: Buffer.from(samples.buffer),
      sampleRate,
      channels: 1,
      hostTimeNs,
      frameCount: frames,
      droppedSamples: 0,
    };
    this.consumer?.(chunk);
  }
}

function collect(mix) {
  const chunks = [];
  mix.on("data", (chunk) => chunks.push(chunk));
  return chunks;
}

void test("mix timestamp-aligned rates, mute microphone, and exclude paused time", async () => {
  const system = new Source();
  const microphone = new Source();
  const mix = new MixedAudioImpl(24000, system, microphone);
  const chunks = collect(mix);
  const ended = once(mix, "end");
  microphone.send(240, 0.5, 24000, 0n);
  system.send(480, 0.25, 48000, 0n);
  mix.pauseCapture();
  mix.setMicrophoneMuted(true);
  mix.resumeCapture();
  system.send(960, 0.25, 48000, 60_000_000_000n);
  microphone.send(480, 0.5, 24000, 60_000_000_000n);
  mix.finishCapture();
  await ended;
  const audio = Buffer.concat(chunks);
  assert.equal(audio.length, 720 * 2);
  for (let frame = 0; frame < 720; frame++) {
    const expected = Math.round(((frame < 240 ? 0.75 : 0.25) / Math.sqrt(2)) * 32767);
    assert.ok(Math.abs(audio.readInt16LE(frame * 2) - expected) <= 1);
  }
});

void test("apply backpressure and preserve the final tail while stopping", async () => {
  const system = new Source();
  const mix = new MixedAudioImpl(24000, system);
  system.send(48000, 0.1, 24000, 0n);
  mix.read(0);
  assert.equal(system.ready, false);
  mix.finishCapture();
  const ended = once(mix, "end");
  const chunks = collect(mix);
  await ended;
  assert.equal(Buffer.concat(chunks).length, 96000);
});

void test("flush upsampling interpolation at the final capture boundary", async () => {
  const system = new Source();
  const mix = new MixedAudioImpl(24000, system);
  const chunks = collect(mix);
  const ended = once(mix, "end");
  system.send(80, 0.25, 8000, 0n);
  mix.finishCapture();
  await ended;
  const audio = Buffer.concat(chunks);
  assert.equal(audio.length, 240 * 2);
  for (let frame = 0; frame < 240; frame++) assert.equal(audio.readInt16LE(frame * 2), 8192);
});

void test("retain an earlier microphone prefix regardless of first packet order in each segment", async () => {
  const system = new Source();
  const microphone = new Source();
  const mix = new MixedAudioImpl(100, system, microphone);
  const chunks = collect(mix);
  const ended = once(mix, "end");
  for (const origin of [0n, 60_000_000_000n]) {
    const previousChunks = chunks.length;
    system.send(10, 0.25, 100, origin + 50_000_000n);
    mix.read(0);
    assert.equal(chunks.length, previousChunks);
    microphone.send(10, 0.5, 100, origin);
    mix.pauseCapture();
    mix.resumeCapture();
  }
  mix.finishCapture();
  await ended;
  const audio = Buffer.concat(chunks);
  assert.equal(audio.length, 30 * 2);
  for (let frame = 0; frame < 30; frame++) {
    const position = frame % 15;
    const value = position < 5 ? 0.5 : position < 10 ? 0.75 : 0.25;
    const expected = Math.round((value / Math.sqrt(2)) * 32767);
    assert.ok(Math.abs(audio.readInt16LE(frame * 2) - expected) <= 1);
  }
});

void test("a missing source delays startup briefly without blocking indefinitely", async () => {
  const system = new Source();
  const mix = new MixedAudioImpl(100, system, new Source());
  system.send(10, 0.25, 100, 0n);
  assert.equal(mix.read(), null);
  await delay(150);
  assert.equal(mix.read()?.length, 20);
  mix.finishCapture();
  mix.destroy();
});

void test("pause and stop flush startup packets without waiting for a missing source", async () => {
  const system = new Source();
  const mix = new MixedAudioImpl(100, system, new Source());
  system.send(10, 0.25, 100, 0n);
  mix.pauseCapture();
  assert.equal(mix.read()?.length, 20);
  mix.resumeCapture();
  system.send(10, 0.25, 100, 60_000_000_000n);
  mix.finishCapture();
  assert.equal(mix.read()?.length, 20);
  mix.destroy();
});

void test("long gaps in every source do not backfill silence on recovery", async () => {
  const system = new Source();
  const microphone = new Source();
  const mix = new MixedAudioImpl(100, system, microphone);
  const chunks = collect(mix);
  const ended = once(mix, "end");
  system.send(10, 0.25, 100, 0n);
  microphone.send(10, 0.25, 100, 0n);
  system.send(10, 0.25, 100, 60_000_000_000n);
  microphone.send(10, 0.25, 100, 60_000_000_000n);
  mix.finishCapture();
  await ended;
  assert.equal(Buffer.concat(chunks).length, 40);
});

void test("a missing source retains silence while another source keeps recording", async () => {
  const system = new Source();
  const microphone = new Source();
  const mix = new MixedAudioImpl(100, system, microphone);
  const chunks = collect(mix);
  const ended = once(mix, "end");
  system.send(10, 0.25, 100, 0n);
  microphone.send(10, 0.25, 100, 0n);
  for (let index = 1; index <= 60; index++)
    system.send(10, 0.25, 100, BigInt(index) * 100_000_000n);
  microphone.send(10, 0.25, 100, 6_000_000_000n);
  mix.finishCapture();
  await ended;
  assert.equal(Buffer.concat(chunks).length, 610 * 2);
});
