import type { AudioChunk } from "./index.js";
import { MonoResampler } from "./mono-resampler.js";

interface MixerInput {
  resampler: MonoResampler;
  cursor: number | null;
  muted: boolean;
}

/** Timestamp-aligned mono s16le, with sparse silence between capture packets. */
export class PcmMixer {
  readonly #sampleRate: number;
  readonly #blockFrames: number;
  readonly #inputs: Map<object, MixerInput>;
  readonly #gain: number;
  readonly #blocks = new Map<number, Float32Array>();
  #origin: bigint | null = null;
  #originFrame = 0;
  #emitted = 0;
  #end = 0;
  #committed = 0;

  constructor(sampleRate: number, sources: readonly object[]) {
    this.#sampleRate = sampleRate;
    this.#blockFrames = Math.max(1, Math.round(sampleRate / 10));
    this.#inputs = new Map(
      sources.map((source) => [
        source,
        {
          resampler: new MonoResampler(sampleRate),
          cursor: null,
          muted: false,
        },
      ]),
    );
    this.#gain = 1 / Math.sqrt(sources.length);
  }

  write(source: object, chunk: AudioChunk): void {
    const input = this.#input(source);
    this.#origin ??= chunk.hostTimeNs;
    let position =
      this.#originFrame +
      Math.round((Number(chunk.hostTimeNs - this.#origin) * this.#sampleRate) / 1e9);
    // When every source was absent for over a second, start a new segment.
    // A live mix must not release minutes of backfilled silence on recovery.
    if (position > this.#end + this.#sampleRate) {
      this.finishSegment();
      this.restart();
      this.#origin = chunk.hostTimeNs;
      position = this.#originFrame;
    }
    const samples = input.resampler.resample(chunk);
    // Absorb up to 20 ms of callback-clock jitter at packet joins.
    if (input.cursor === null || Math.abs(position - input.cursor) > this.#sampleRate / 50) {
      input.cursor = position;
    }
    this.#append(input, samples);
  }

  #append(input: MixerInput, samples: Float32Array): void {
    if (input.cursor === null) return;
    const start = input.cursor;
    input.cursor += samples.length;
    this.#end = Math.max(this.#end, input.cursor);
    if (input.muted) return;
    for (let index = Math.max(0, this.#emitted - start); index < samples.length; index++) {
      const frame = start + index;
      const blockIndex = Math.floor(frame / this.#blockFrames);
      let block = this.#blocks.get(blockIndex);
      if (!block) {
        block = new Float32Array(this.#blockFrames);
        this.#blocks.set(blockIndex, block);
      }
      const offset = frame % this.#blockFrames;
      block[offset] = block[offset]! + samples[index]! * this.#gain;
    }
  }

  setMuted(source: object, muted: boolean): void {
    this.#input(source).muted = muted;
  }

  /** Makes the remaining captured tail readable before pause or stop. */
  finishSegment(): void {
    for (const input of this.#inputs.values()) {
      this.#append(input, input.resampler.flush());
    }
    this.#committed = this.#end;
  }

  /** Keeps unread audio, while removing paused time from the next segment. */
  restart(): void {
    this.#origin = null;
    this.#originFrame = this.#end;
    for (const input of this.#inputs.values()) {
      input.cursor = null;
      input.resampler.reset();
    }
  }

  read(): Buffer | null {
    const cursors = [...this.#inputs.values()]
      .map((input) => input.cursor)
      .filter((cursor) => cursor !== null);
    let ready = this.#committed;
    if (cursors.length > 0) {
      const lead = Math.max(...cursors);
      // A track more than half a second behind contributes silence.
      ready = Math.max(
        ready,
        Math.min(...cursors.filter((cursor) => lead - cursor <= this.#sampleRate / 2)),
      );
    }
    const end = Math.min(ready, this.#emitted + this.#blockFrames);
    if (end <= this.#emitted) return null;
    const output = Buffer.allocUnsafe((end - this.#emitted) * 2);
    for (let frame = this.#emitted; frame < end; frame++) {
      const block = this.#blocks.get(Math.floor(frame / this.#blockFrames));
      const sample = Math.max(-1, Math.min(1, block?.[frame % this.#blockFrames] ?? 0));
      output.writeInt16LE(
        Math.round(sample < 0 ? sample * 0x8000 : sample * 0x7fff),
        (frame - this.#emitted) * 2,
      );
    }
    for (const blockIndex of this.#blocks.keys()) {
      if ((blockIndex + 1) * this.#blockFrames <= end) this.#blocks.delete(blockIndex);
    }
    this.#emitted = end;
    return output;
  }

  #input(source: object): MixerInput {
    const input = this.#inputs.get(source);
    if (!input) throw new Error("Not a source of this mixer");
    return input;
  }
}

export function getLevel(pcm: Buffer): number {
  let sum = 0;
  for (let offset = 0; offset < pcm.length; offset += 2) {
    const sample = pcm.readInt16LE(offset) / 0x8000;
    sum += sample * sample;
  }
  return Math.sqrt(sum / (pcm.length / 2));
}
