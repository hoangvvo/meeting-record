import assert from "node:assert/strict";
import test from "node:test";
import { createMixer } from "./helpers/native-mixer.mjs";

function chunk(samples, rate, channels = 1) {
  return {
    pcm: Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength),
    channels,
    sampleRate: rate,
    hostTimeNs: 0n,
    frameCount: samples.length / channels,
  };
}

function convert(samples, rate, split = samples.length, channels = 1) {
  const mixer = createMixer(16000);
  for (let offset = 0; offset < samples.length; offset += split) {
    const packet = chunk(samples.subarray(offset, offset + split), rate, channels);
    packet.hostTimeNs = BigInt(Math.round(((offset / channels) * 1e9) / rate));
    mixer.write(0, packet);
  }
  mixer.finishSegment();
  const output = drain(mixer);
  mixer.finishSegment();
  assert.deepEqual(drain(mixer), []);
  mixer.close();
  return output;
}

function drain(mixer) {
  const output = [];
  for (let packet; (packet = mixer.read()) !== null;) {
    for (let offset = 0; offset < packet.pcm.length; offset += 2)
      output.push(packet.pcm.readInt16LE(offset) / 32768);
  }
  return output;
}

void test("native mixer copies unaligned input safely and rejects malformed packets", () => {
  const mixer = createMixer(16000);
  const pcm = Buffer.alloc(4 * 160 + 1).subarray(1);
  for (let frame = 0; frame < 160; frame++) pcm.writeFloatLE(0.25, frame * 4);
  mixer.write(0, { pcm, channels: 1, sampleRate: 16000, hostTimeNs: 0n });
  assert.deepEqual(
    drain(mixer),
    Array.from({ length: 160 }, () => 0.25),
  );
  assert.throws(
    () => mixer.write(0, { pcm: pcm.subarray(1), channels: 1, sampleRate: 16000, hostTimeNs: 0n }),
    /layout/,
  );
  mixer.close();
  mixer.close();
  assert.throws(() => mixer.read(), /closed/);
});

void test("downsampling suppresses aliases without attenuating speech-band tones", () => {
  for (const rate of [44100, 48000, 96000]) {
    for (const frequency of [1000, 10000]) {
      const input = Float32Array.from({ length: rate }, (_, i) =>
        Math.sin((2 * Math.PI * frequency * i) / rate),
      );
      const output = convert(input, rate, 257);
      assert.equal(output.length, 16000);
      const alias = frequency < 8000 ? frequency : 16000 - frequency;
      let real = 0,
        imaginary = 0;
      for (let i = 200; i < output.length - 200; i++) {
        const angle = (2 * Math.PI * alias * i) / 16000;
        real += output[i] * Math.cos(angle);
        imaginary += output[i] * Math.sin(angle);
      }
      const amplitude = (2 * Math.hypot(real, imaginary)) / (output.length - 400);
      assert.ok(
        frequency < 8000 ? Math.abs(amplitude - 1) < 0.001 : amplitude < 0.001,
        `rate ${rate}, frequency ${frequency}, amplitude ${amplitude}`,
      );
    }
  }
});

void test("filter history and final duration are independent of stereo chunk boundaries", () => {
  for (const rate of [44100, 48000, 96000]) {
    const input = Float32Array.from({ length: 2002 }, (_, i) => Math.sin(i * 0.19));
    const whole = convert(input, rate, input.length, 2);
    const split = convert(input, rate, 14, 2);
    assert.equal(split.length, Math.ceil((1001 * 16000) / rate));
    assert.deepEqual(split, whole);
  }
});

void test("format changes flush filter lookahead before the new rate", () => {
  const mixer = createMixer(16000);
  mixer.write(0, chunk(new Float32Array(301).fill(0.25), 48000));
  mixer.write(0, chunk(new Float32Array(160).fill(0.5), 16000));
  mixer.finishSegment();
  const output = drain(mixer);
  mixer.close();
  assert.equal(output.length, 101 + 160);
  assert.ok(output.slice(0, 101).every((value) => Math.abs(value - 0.25) < 1e-6));
  assert.ok(output.slice(101).every((value) => value === 0.5));
});

void test("a timestamp gap retains the old filter tail and starts a fresh filter at the new packet", () => {
  for (const rate of [44100, 48000, 96000]) {
    const mixer = createMixer(16000);
    const first = chunk(new Float32Array(rate / 10).fill(0.25), rate);
    const second = chunk(new Float32Array(rate / 10).fill(0.5), rate);
    second.hostTimeNs = 500_000_000n;
    mixer.write(0, first);
    mixer.write(0, second);
    mixer.finishSegment();
    const packets = [];
    for (let packet; (packet = mixer.read()) !== null;) packets.push(packet.pcm);
    mixer.close();
    const output = Buffer.concat(packets);
    assert.equal(output.length / 2, 9600, `duration at ${rate} Hz`);
    for (let frame = 0; frame < 9600; frame++) {
      const expected = frame < 1600 ? 8192 : frame < 8000 ? 0 : 16384;
      assert.equal(output.readInt16LE(frame * 2), expected, `frame ${frame} at ${rate} Hz`);
    }
  }
});
