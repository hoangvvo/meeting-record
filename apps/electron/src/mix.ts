/**
 * Offline mixdown of the two capture stems into one mono file.
 *
 * Streams the stems rather than loading them: an hour of 48 kHz audio is ~700 MB
 * as float32, and meetings run that long. Costs a second read of each stem,
 * because the peak has to be known before any sample can be scaled.
 */
import { open, type FileHandle } from "node:fs/promises";
import { WavWriter } from "./wav";

const BLOCK_FRAMES = 8192;

interface Format {
  readonly sampleRate: number;
  readonly channels: number;
  readonly bitsPerSample: number;
  readonly float: boolean;
  readonly dataStart: number;
  readonly dataEnd: number;
}

/** Walks the RIFF chunk list for `fmt ` and `data`. */
async function readFormat(handle: FileHandle): Promise<Format> {
  const head = Buffer.alloc(12);
  await handle.read(head, 0, 12, 0);
  if (head.toString("ascii", 0, 4) !== "RIFF" || head.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error("not a RIFF/WAVE file");
  }

  let offset = 12;
  let format: Omit<Format, "dataStart" | "dataEnd"> | undefined;
  const header = Buffer.alloc(8);
  for (;;) {
    const { bytesRead } = await handle.read(header, 0, 8, offset);
    if (bytesRead < 8) break;
    const id = header.toString("ascii", 0, 4);
    const size = header.readUInt32LE(4);
    const body = offset + 8;

    if (id === "fmt ") {
      const chunk = Buffer.alloc(Math.min(size, 16));
      await handle.read(chunk, 0, chunk.byteLength, body);
      const tag = chunk.readUInt16LE(0);
      format = {
        channels: Math.max(1, chunk.readUInt16LE(2)),
        sampleRate: chunk.readUInt32LE(4),
        bitsPerSample: chunk.readUInt16LE(14),
        float: tag === 3,
      };
    } else if (id === "data") {
      if (!format) throw new Error("data chunk came before fmt");
      return { ...format, dataStart: body, dataEnd: body + size };
    }

    // chunks are word-aligned, odd sizes carry a pad byte
    offset = body + size + (size % 2);
  }
  throw new Error("no data chunk");
}

/**
 * One stem, read forward only. Holds just the frames the current block needs,
 * which is what keeps this a stream instead of a buffer.
 */
class Stem {
  readonly sampleRate: number;
  readonly frameCount: number;
  readonly #handle: FileHandle;
  readonly #format: Format;
  readonly #frameBytes: number;
  #cursor: number;
  #frames = new Float32Array(0);
  #base = 0;

  private constructor(handle: FileHandle, format: Format) {
    this.#handle = handle;
    this.#format = format;
    this.#frameBytes = (format.bitsPerSample / 8) * format.channels;
    this.#cursor = format.dataStart;
    this.sampleRate = format.sampleRate || 1;
    this.frameCount = Math.floor((format.dataEnd - format.dataStart) / this.#frameBytes);
  }

  static async open(path: string): Promise<Stem> {
    const handle = await open(path, "r");
    try {
      return new Stem(handle, await readFormat(handle));
    } catch (error) {
      await handle.close();
      throw error;
    }
  }

  get seconds(): number {
    return this.frameCount / this.sampleRate;
  }

  /** Buffer everything `at()` will ask for between `from` and `to` seconds. */
  async cover(from: number, to: number): Promise<void> {
    const first = Math.max(0, Math.floor(from * this.sampleRate));
    // +1 for the frame that interpolation reads ahead
    const last = Math.floor(to * this.sampleRate) + 1;
    if (first >= this.#base && last < this.#base + this.#frames.length) return;

    const keepFrom = Math.max(first, this.#base);
    const kept = this.#frames.subarray(Math.min(keepFrom - this.#base, this.#frames.length));
    const wanted = Math.max(last - keepFrom + 1, kept.length);
    // reads past the end of the stem stay zero, so a short stem fades to silence
    const frames = new Float32Array(wanted);
    frames.set(kept);

    let filled = kept.length;
    while (filled < wanted && this.#cursor < this.#format.dataEnd) {
      const bytes = Math.min(
        (wanted - filled) * this.#frameBytes,
        this.#format.dataEnd - this.#cursor,
      );
      const chunk = Buffer.allocUnsafe(bytes);
      const { bytesRead } = await this.#handle.read(chunk, 0, bytes, this.#cursor);
      if (bytesRead === 0) break;
      this.#cursor += bytesRead;
      filled += this.#decode(chunk.subarray(0, bytesRead), frames, filled);
    }

    this.#frames = frames;
    this.#base = keepFrom;
  }

  /** Interleaved samples to mono frames. Returns how many frames it wrote. */
  #decode(chunk: Buffer, into: Float32Array, at: number): number {
    const { channels, float, bitsPerSample } = this.#format;
    const width = bitsPerSample / 8;
    const frames = Math.floor(chunk.byteLength / this.#frameBytes);
    for (let frame = 0; frame < frames; frame++) {
      let sum = 0;
      for (let channel = 0; channel < channels; channel++) {
        const offset = frame * this.#frameBytes + channel * width;
        sum += float ? chunk.readFloatLE(offset) : chunk.readInt16LE(offset) / 32767;
      }
      into[at + frame] = sum / channels;
    }
    return frames;
  }

  /** The value at `seconds`, linearly interpolated. Silence outside the stem. */
  at(seconds: number): number {
    const position = seconds * this.sampleRate;
    const index = Math.floor(position);
    const current = this.#frame(index);
    return current + (this.#frame(index + 1) - current) * (position - index);
  }

  #frame(index: number): number {
    const offset = index - this.#base;
    return offset >= 0 && offset < this.#frames.length ? this.#frames[offset]! : 0;
  }

  async close(): Promise<void> {
    await this.#handle.close();
  }
}

async function eachBlock(
  systemPath: string,
  micPath: string | null,
  rate: number,
  frames: number,
  visit: (block: Float32Array) => Promise<void> | void,
): Promise<void> {
  const system = await Stem.open(systemPath);
  const microphone = micPath === null ? null : await Stem.open(micPath);
  const block = new Float32Array(BLOCK_FRAMES);

  try {
    for (let start = 0; start < frames; start += BLOCK_FRAMES) {
      const end = Math.min(start + BLOCK_FRAMES, frames);
      await system.cover(start / rate, (end - 1) / rate);
      await microphone?.cover(start / rate, (end - 1) / rate);

      for (let frame = start; frame < end; frame++) {
        const at = frame / rate;
        block[frame - start] = system.at(at) + (microphone?.at(at) ?? 0);
      }
      await visit(block.subarray(0, end - start));
    }
  } finally {
    await system.close();
    await microphone?.close();
  }
}

/**
 * Sums the stems into `outPath` at the system stem's rate, resampling the mic to
 * match. Returns the peak of the mix before any scaling.
 *
 * The two tracks run off independent clocks, so they are aligned at the start
 * and drift apart by whatever those clocks disagree on. Fine to listen back to,
 * not sample-accurate.
 */
export async function mix(
  systemPath: string,
  micPath: string | null,
  outPath: string,
): Promise<number> {
  const system = await Stem.open(systemPath);
  const rate = system.sampleRate;
  let seconds = system.seconds;
  await system.close();

  if (micPath !== null) {
    const microphone = await Stem.open(micPath);
    seconds = Math.max(seconds, microphone.seconds);
    await microphone.close();
  }
  const frames = Math.ceil(seconds * rate);

  let peak = 0;
  await eachBlock(systemPath, micPath, rate, frames, (block) => {
    for (const sample of block) {
      const magnitude = Math.abs(sample);
      if (magnitude > peak) peak = magnitude;
    }
  });

  // summing two loud tracks clips, so scale the mix back by exactly what it
  // overshot instead of clamping every sample
  const gain = peak > 1 ? 1 / peak : 1;
  const writer = await WavWriter.create(outPath, rate, 1);
  const scaled = new Float32Array(BLOCK_FRAMES);
  await eachBlock(systemPath, micPath, rate, frames, (block) => {
    for (let index = 0; index < block.length; index++) scaled[index] = block[index]! * gain;
    const view = scaled.subarray(0, block.length);
    writer.write(Buffer.from(view.buffer, view.byteOffset, view.byteLength));
  });
  await writer.finish();

  return peak;
}
