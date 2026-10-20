// Windows live-audio E2E harness for the Node.js binding. It mirrors
// e2e/windows/src/main.rs and reads the same environment variables:
//
// - MREC_TARGET_PID: capture this process tree instead of system audio.
// - MREC_DURATION_SECONDS: capture duration (default: 6).
// - MREC_MONO=1: request a mono system-audio track instead of stereo.
// - MREC_MICROPHONE=1: require a non-silent default-microphone track.
// - MREC_EXPECT_RECOVERY=1: require an interrupted -> recovered transition.
// - MREC_MIN_PEAK: minimum absolute sample peak (default: 0.0001).
// - MREC_ALLOW_DROPS=1: permit bounded-queue drops.
// - MREC_EXPECT_FREQUENCIES: comma-separated tones (Hz) the system track must
//   carry without dropouts.
// - MREC_REJECT_FREQUENCIES: comma-separated tones (Hz) the system track must not
//   carry, such as another process's audio.
// - MREC_MICROPHONE_FREQUENCIES: comma-separated tones (Hz) the microphone track
//   must carry without dropouts. The mixed output must carry those the system
//   track rejects; one it also carries may cancel out.
// - MREC_MIX_SAMPLE_RATE: also read the mixed output at this rate.
// - MREC_PAUSE_SECONDS: pause for this long mid-capture; paused audio must be
//   omitted from every track.
// - MREC_RESTART_CYCLES: start and stop this many short sessions first.
// - MREC_EXPECT_MEETING: a platform id such as `meet`. Scanning and watching must
//   both report a recordable meeting there, which becomes the capture target.
//   With MREC_TARGET_PID, the meeting must belong to that process.
import { on, once } from "node:events";
import { setTimeout as delay } from "node:timers/promises";

import * as MeetingRecord from "meeting-record";

/** A timestamp further than this from the previous chunk's end is a gap. */
const CONTINUITY_TOLERANCE_NS = 2_000_000n;
/** A tone below this normalised power ratio is absent. */
const ABSENT_TONE_RATIO = 0.05;
/** An expected tone must reach this ratio; two equal tones each measure 0.5. */
const PRESENT_TONE_RATIO = 0.1;
/** Samples below this level count towards a dropout. */
const SILENCE_LEVEL = 1e-4;
/** Silence longer than this, in seconds, inside a continuous tone is a dropout. */
const MIN_DROPOUT_SECONDS = 0.001;
/**
 * Capture devices, virtual cables especially, drop a period on their own now and
 * then. Misplaced packets drop many.
 */
const MAX_MICROPHONE_DROPOUTS = 2;
/**
 * Sources start with silence, sometimes broken by a fragment: process loopback until
 * the engine attaches the target's stream, a capture device while it fills. Both
 * bindings' mixers also wait this long, in seconds, for every source.
 */
const STARTUP_SECONDS = 0.1;

class TrackMetrics {
  frames = 0;
  samples = 0;
  seconds = 0;
  peak = 0;
  droppedSamples = 0;
  malformedChunks = 0;
  timestampRegressions = 0;
  /** @type {bigint[]} */
  gapsNs = [];
  /** @type {bigint | undefined} */
  firstHostTimeNs;
  /** @type {bigint | undefined} */
  lastHostTimeNs;
  /** @type {bigint | undefined} */
  nextHostTimeNs;
  sampleRate = 0;
  /** @type {number[]} */
  mono = [];

  /** @param {MeetingRecord.AudioChunk} chunk */
  add(chunk) {
    const frames = new Float32Array(chunk.buffer, chunk.byteOffset, chunk.byteLength / 4);
    if (
      frames.length !== chunk.frameCount * chunk.channels ||
      chunk.channels === 0 ||
      chunk.sampleRate <= 0
    ) {
      this.malformedChunks++;
      return;
    }
    if (this.lastHostTimeNs === undefined) {
      this.firstHostTimeNs = chunk.hostTimeNs;
    } else if (chunk.hostTimeNs < this.lastHostTimeNs) {
      this.timestampRegressions++;
    }
    if (this.nextHostTimeNs !== undefined) {
      const distance = chunk.hostTimeNs - this.nextHostTimeNs;
      if (distance > CONTINUITY_TOLERANCE_NS || -distance > CONTINUITY_TOLERANCE_NS) {
        this.gapsNs.push(distance > 0n ? distance : 0n);
      }
    }
    this.nextHostTimeNs =
      chunk.hostTimeNs + BigInt(Math.round((chunk.frameCount * 1e9) / chunk.sampleRate));
    this.lastHostTimeNs = chunk.hostTimeNs;
    this.frames += chunk.frameCount;
    this.samples += frames.length;
    this.seconds += chunk.frameCount / chunk.sampleRate;
    this.droppedSamples += chunk.droppedSamples;
    this.sampleRate = chunk.sampleRate;
    for (let frame = 0; frame < chunk.frameCount; frame++) {
      let sum = 0;
      for (let channel = 0; channel < chunk.channels; channel++) {
        const sample = frames[frame * chunk.channels + channel];
        this.peak = Math.max(this.peak, Math.abs(sample));
        sum += sample;
      }
      this.mono.push(sum / chunk.channels);
    }
  }

  verify(name, expectedSeconds, expectations) {
    const minimumSeconds = expectedSeconds * 0.7;
    if (this.frames === 0 || this.seconds < minimumSeconds) {
      throw new Error(
        `${name}: captured only ${this.seconds.toFixed(2)}s (${this.frames}) frames; expected at least ${minimumSeconds.toFixed(2)}s`,
      );
    }
    if (this.peak < expectations.minimumPeak) {
      throw new Error(
        `${name}: peak ${this.peak.toFixed(6)} is below required ${expectations.minimumPeak.toFixed(6)}`,
      );
    }
    if (this.firstHostTimeNs === 0n) throw new Error(`${name}: first host timestamp is zero`);
    if (this.malformedChunks !== 0) {
      throw new Error(`${name}: ${this.malformedChunks} chunks had invalid frame/format metadata`);
    }
    if (this.timestampRegressions !== 0) {
      throw new Error(`${name}: ${this.timestampRegressions} timestamp regressions`);
    }
    if (!expectations.allowDrops && this.droppedSamples !== 0) {
      throw new Error(`${name}: ${this.droppedSamples} samples were dropped`);
    }
    verifyGaps(name, this.gapsNs, expectations.pauseSeconds);
  }

  report(name) {
    console.log(
      `${name}: ${this.seconds.toFixed(2)}s, ${this.frames} frames, ${this.samples} samples, peak=${this.peak.toFixed(6)}, dropped=${this.droppedSamples}, gaps=${formatGaps(this.gapsNs)}, timestamps=${this.firstHostTimeNs ?? 0n}..${this.lastHostTimeNs ?? 0n}`,
    );
  }
}

function formatGaps(gapsNs) {
  return `[${gapsNs.map((gap) => `${(Number(gap) / 1e9).toFixed(2)}s`).join(", ")}]`;
}

/** Every gap must come from the pause: exactly one, close to its length. */
function verifyGaps(name, gapsNs, pauseSeconds) {
  const matches =
    pauseSeconds === undefined
      ? gapsNs.length === 0
      : gapsNs.length === 1 && Number(gapsNs[0]) / 1e9 >= pauseSeconds * 0.5;
  if (!matches) {
    throw new Error(
      `${name}: timestamp gaps ${formatGaps(gapsNs)} do not match pause ${pauseSeconds ?? "none"}`,
    );
  }
}

/**
 * Normalised single-bin DFT power per 100 ms block, averaged over blocks with
 * signal: about 1 for a pure tone at `frequency`, 0 for unrelated audio.
 */
function toneRatio(samples, sampleRate, frequency) {
  const block = Math.max(1, Math.floor(sampleRate / 10));
  const coefficient = 2 * Math.cos((2 * Math.PI * frequency) / sampleRate);
  let total = 0;
  let blocks = 0;
  for (let start = 0; start + block <= samples.length; start += block) {
    let previous = 0;
    let beforePrevious = 0;
    let energy = 0;
    for (let index = start; index < start + block; index++) {
      const current = samples[index] + coefficient * previous - beforePrevious;
      beforePrevious = previous;
      previous = current;
      energy += samples[index] * samples[index];
    }
    if (energy > 1e-9) {
      const power =
        previous * previous +
        beforePrevious * beforePrevious -
        coefficient * previous * beforePrevious;
      total += (2 * power) / (block * energy);
      blocks++;
    }
  }
  return blocks === 0 ? 0 : total / blocks;
}

/**
 * Silent runs, in seconds, between the first and last audible samples. Capture may
 * start or stop between a source's packets.
 */
function dropouts(samples, sampleRate) {
  const audible = (sample) => Math.abs(sample) >= SILENCE_LEVEL;
  const start = samples.findIndex(audible);
  const end = samples.findLastIndex(audible);
  const found = [];
  if (start < 0) return found;
  let run = 0;
  for (let index = start; index <= end; index++) {
    if (!audible(samples[index])) {
      run++;
    } else {
      if (run / sampleRate > MIN_DROPOUT_SECONDS) found.push(run / sampleRate);
      run = 0;
    }
  }
  return found;
}

function verifyTones(name, samples, sampleRate, expected, rejected) {
  for (const frequency of expected) {
    const ratio = toneRatio(samples, sampleRate, frequency);
    console.log(`${name}: ${frequency} Hz ratio ${ratio.toFixed(3)}`);
    if (ratio < PRESENT_TONE_RATIO) {
      throw new Error(`${name}: expected ${frequency} Hz, measured ratio ${ratio.toFixed(3)}`);
    }
  }
  for (const frequency of rejected) {
    const ratio = toneRatio(samples, sampleRate, frequency);
    console.log(`${name}: ${frequency} Hz ratio ${ratio.toFixed(3)} (rejected)`);
    if (ratio > ABSENT_TONE_RATIO) {
      throw new Error(
        `${name}: captured ${frequency} Hz from outside the target (ratio ${ratio.toFixed(3)})`,
      );
    }
  }
}

/** A continuous tone must not fall silent after startup more than `allowed` times. */
function verifyContinuous(name, samples, sampleRate, allowed) {
  const found = dropouts(samples.slice(Math.floor(STARTUP_SECONDS * sampleRate)), sampleRate);
  if (found.length > allowed) {
    const list = found.map((seconds) => `${(seconds * 1000).toFixed(1)}ms`).join(", ");
    throw new Error(`${name}: continuous tone has dropouts [${list}]; at most ${allowed} allowed`);
  }
}

class MixMetrics {
  bytes = 0;
  peakLevel = 0;
  /** @type {number[]} */
  samples = [];

  verify(sampleRate, capturedSeconds, expectations) {
    const seconds = this.samples.length / sampleRate;
    console.log(
      `mixed: ${seconds.toFixed(2)}s at ${sampleRate} Hz, ${this.bytes} bytes, peak level=${this.peakLevel.toFixed(6)}`,
    );
    if (this.bytes % 2 !== 0)
      throw new Error(`mixed: ${this.bytes} bytes is not whole s16le samples`);
    if (Math.abs(seconds - capturedSeconds) > 0.25) {
      throw new Error(
        `mixed: ${seconds.toFixed(2)}s does not match ${capturedSeconds.toFixed(2)}s of system audio`,
      );
    }
    if (this.peakLevel <= 0) throw new Error("mixed: level never rose above zero");
    // Every source's tones reach the mix, unless they are above its Nyquist rate.
    // A microphone tone the system track also carries may cancel.
    const audible = [
      ...expectations.expectedFrequencies,
      ...expectations.microphoneFrequencies.filter((frequency) =>
        expectations.rejectedFrequencies.includes(frequency),
      ),
    ];
    verifyTones(
      "mixed",
      this.samples,
      sampleRate,
      audible.filter((frequency) => frequency < sampleRate / 2),
      [],
    );
    // The system tones keep the mix audible through any microphone dropout.
    if (expectations.expectedFrequencies.length > 0) {
      verifyContinuous("mixed", this.samples, sampleRate, 0);
    }
  }
}

const enabled = (name) => ["1", "true", "yes"].includes(process.env[name] ?? "");

function number(name, fallback) {
  const value = process.env[name];
  if (value === undefined) return fallback;
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) throw new TypeError(`${name} must be a number: ${value}`);
  return parsed;
}

function frequencies(name) {
  return (process.env[name] ?? "")
    .split(",")
    .filter((item) => item.trim() !== "")
    .map((item) => {
      const frequency = Number(item);
      if (!Number.isFinite(frequency))
        throw new TypeError(`${name} has an invalid frequency: ${item}`);
      return frequency;
    });
}

/** Resolves once every listed stream has ended. */
const ended = (streams) =>
  Promise.all(streams.filter(Boolean).map((stream) => once(stream, "end")));

/** The app of a meeting that both a scan and the watcher report as recordable. */
async function findMeeting(platform, pid) {
  const found = MeetingRecord.meetings.scan();
  for (const meeting of found) console.log(`scan: ${JSON.stringify(meeting)}`);
  const meeting = found.find(
    (candidate) =>
      candidate.platform === platform &&
      candidate.shouldRecord &&
      (pid === undefined || candidate.pid === pid),
  );
  if (!meeting) throw new Error(`no recordable ${platform} meeting was detected`);

  // The watcher reports the current state as soon as it starts.
  const reported = (async () => {
    const events = on(MeetingRecord.meetings, "started", { signal: AbortSignal.timeout(5000) });
    for await (const [candidate] of events) {
      if (candidate.pid === meeting.pid) return candidate;
    }
  })();
  MeetingRecord.meetings.watch();
  let watched;
  try {
    watched = await reported;
  } catch {
    throw new Error(`the watcher did not report the ${platform} meeting`);
  } finally {
    MeetingRecord.meetings.unwatch();
  }
  if (watched.platform !== meeting.platform || !watched.shouldRecord) {
    throw new Error(
      `the watcher reported ${JSON.stringify(watched)}, but a scan found ${JSON.stringify(meeting)}`,
    );
  }
  console.log(
    `meeting: ${meeting.platform} pid ${meeting.pid} ${JSON.stringify(meeting.title)} ${meeting.url}`,
  );
  return { type: "process", pid: meeting.pid };
}

/** Short sessions must each deliver audio, stop cleanly, and leave capture idle. */
async function restartCycles(target, options, cycles) {
  for (let cycle = 1; cycle <= cycles; cycle++) {
    const recording = await MeetingRecord.capture.start(target, options);
    let frames = 0;
    recording.systemAudio.on("data", (chunk) => (frames += chunk.frameCount));
    recording.mixedAudio?.resume();
    const finished = ended([recording.systemAudio, recording.mixedAudio]);
    await delay(250);
    await recording.stopRecording();
    await finished;
    if (frames === 0) throw new Error(`restart cycle ${cycle}: no audio before stop`);
    if (recording.health !== "stopped" || MeetingRecord.capture.running) {
      throw new Error(`restart cycle ${cycle}: capture still running after stop`);
    }
  }
  if (cycles > 0) console.log(`restart cycles: ${cycles} passed`);
}

async function main() {
  const durationSeconds = number("MREC_DURATION_SECONDS", 6);
  const mixSampleRate =
    process.env.MREC_MIX_SAMPLE_RATE === undefined ? undefined : number("MREC_MIX_SAMPLE_RATE");
  const pauseSeconds =
    process.env.MREC_PAUSE_SECONDS === undefined ? undefined : number("MREC_PAUSE_SECONDS");
  const targetPid = number("MREC_TARGET_PID", undefined);
  const target = process.env.MREC_EXPECT_MEETING
    ? await findMeeting(process.env.MREC_EXPECT_MEETING, targetPid)
    : targetPid === undefined
      ? { type: "system" }
      : { type: "process", pid: targetPid };
  const includeMicrophone = enabled("MREC_MICROPHONE");
  const expectRecovery = enabled("MREC_EXPECT_RECOVERY");
  const expectations = {
    minimumPeak: number("MREC_MIN_PEAK", 0.0001),
    allowDrops: enabled("MREC_ALLOW_DROPS"),
    pauseSeconds,
    expectedFrequencies: frequencies("MREC_EXPECT_FREQUENCIES"),
    rejectedFrequencies: frequencies("MREC_REJECT_FREQUENCIES"),
    microphoneFrequencies: frequencies("MREC_MICROPHONE_FREQUENCIES"),
  };
  const options = {
    mono: enabled("MREC_MONO"),
    ...(includeMicrophone ? { microphone: "default" } : {}),
    ...(mixSampleRate === undefined ? {} : { mix: { sampleRate: mixSampleRate } }),
  };

  await restartCycles(target, options, number("MREC_RESTART_CYCLES", 0));

  console.log(`starting ${JSON.stringify(target)} capture for ${durationSeconds}s`);
  const recording = await MeetingRecord.capture.start(target, options);
  const system = new TrackMetrics();
  const microphone = includeMicrophone ? new TrackMetrics() : undefined;
  const mixed = mixSampleRate === undefined ? undefined : new MixMetrics();
  recording.systemAudio.on("data", (chunk) => system.add(chunk));
  if (microphone) {
    if (!recording.microphone) throw new Error("microphone track requested but missing");
    recording.microphone.on("data", (chunk) => microphone.add(chunk));
  }
  if (mixed) {
    if (!recording.mixedAudio) throw new Error("mixed audio requested but missing");
    recording.mixedAudio.on("data", (chunk) => {
      mixed.bytes += chunk.length;
      for (let offset = 0; offset + 1 < chunk.length; offset += 2) {
        mixed.samples.push(chunk.readInt16LE(offset) / 32768);
      }
    });
    recording.mixedAudio.on(
      "level",
      (level) => (mixed.peakLevel = Math.max(mixed.peakLevel, level)),
    );
  }
  let sawInterrupted = false;
  let sawRecovered = false;
  recording.on("interrupted", () => (sawInterrupted = true));
  recording.on("recovered", () => (sawRecovered = true));
  const finished = ended([recording.systemAudio, recording.microphone, recording.mixedAudio]);

  const started = performance.now();
  const deadline = started + durationSeconds * 1000;
  // Pause once, in the middle of the recording.
  let pauseWindow =
    pauseSeconds === undefined
      ? undefined
      : {
          start: started + ((durationSeconds - pauseSeconds) * 1000) / 2,
          end: started + ((durationSeconds + pauseSeconds) * 1000) / 2,
        };
  while (performance.now() < deadline) {
    await delay(50);
    if (pauseWindow) {
      const now = performance.now();
      if (now >= pauseWindow.end) {
        recording.resumeRecording();
        pauseWindow = undefined;
      } else if (now >= pauseWindow.start && recording.state === "recording") {
        recording.pauseRecording();
      }
    }
    if (recording.health === "failed") break;
  }
  const terminalFailure = recording.failure;
  await recording.stopRecording();
  await finished;

  if (terminalFailure) throw terminalFailure;
  if (expectRecovery && (!sawInterrupted || !sawRecovered)) {
    throw new Error("expected a complete interrupted -> recovered transition");
  }

  const expectedSeconds = durationSeconds - (pauseSeconds ?? 0);
  system.report("system");
  system.verify("system", expectedSeconds, expectations);
  verifyTones(
    "system",
    system.mono,
    system.sampleRate,
    expectations.expectedFrequencies,
    expectations.rejectedFrequencies,
  );
  if (expectations.expectedFrequencies.length > 0) {
    verifyContinuous("system", system.mono, system.sampleRate, 0);
  }
  if (microphone) {
    microphone.report("microphone");
    microphone.verify("microphone", expectedSeconds, expectations);
    verifyTones(
      "microphone",
      microphone.mono,
      microphone.sampleRate,
      expectations.microphoneFrequencies,
      [],
    );
    if (expectations.microphoneFrequencies.length > 0) {
      verifyContinuous(
        "microphone",
        microphone.mono,
        microphone.sampleRate,
        MAX_MICROPHONE_DROPOUTS,
      );
    }
  }
  if (mixed) mixed.verify(mixSampleRate, system.seconds, expectations);
  console.log("PASS: live audio capture met all E2E assertions");
}

try {
  await main();
} catch (error) {
  console.error(`Error: ${error instanceof Error ? error.message : String(error)}`);
  process.exitCode = 1;
}
