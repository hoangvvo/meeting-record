// RIFF sizes live in the header and aren't known until the end, so we write it
// zeroed and patch it in finish(). a crash mid-recording therefore leaves a file
// claiming zero length
import { createWriteStream, type WriteStream } from "node:fs";
import { open } from "node:fs/promises";
import { once } from "node:events";

const HEADER_BYTES = 44;
const BYTES_PER_SAMPLE = 2;

function header(sampleRate: number, channels: number, dataBytes: number): Buffer {
  const blockAlign = channels * BYTES_PER_SAMPLE;
  const buffer = Buffer.alloc(HEADER_BYTES);
  buffer.write("RIFF", 0);
  buffer.writeUInt32LE(36 + dataBytes, 4);
  buffer.write("WAVE", 8);
  buffer.write("fmt ", 12);
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20); // uncompressed PCM
  buffer.writeUInt16LE(channels, 22);
  buffer.writeUInt32LE(sampleRate, 24);
  buffer.writeUInt32LE(sampleRate * blockAlign, 28);
  buffer.writeUInt16LE(blockAlign, 32);
  buffer.writeUInt16LE(8 * BYTES_PER_SAMPLE, 34);
  buffer.write("data", 36);
  buffer.writeUInt32LE(dataBytes, 40);
  return buffer;
}

export class WavWriter {
  #stream: WriteStream;
  #dataBytes = 0;

  private constructor(
    readonly path: string,
    readonly sampleRate: number,
    readonly channels: number,
    stream: WriteStream,
  ) {
    this.#stream = stream;
  }

  static async create(path: string, sampleRate: number, channels: number): Promise<WavWriter> {
    const rate = Math.round(sampleRate);
    const stream = createWriteStream(path);
    await once(stream, "open");
    stream.write(header(rate, channels, 0));
    return new WavWriter(path, rate, channels, stream);
  }

  get seconds(): number {
    return this.#dataBytes / BYTES_PER_SAMPLE / this.channels / this.sampleRate;
  }

  /** Converts interleaved float32 to int16 and returns the peak magnitude seen. */
  write(chunk: Buffer): number {
    // the capture stream can still flush buffered chunks after stop(), by which
    // point the file is already closing
    if (this.#stream.writableEnded) return 0;

    // readFloatLE rather than a Float32Array view: chunks off the addon aren't
    // guaranteed 4-byte aligned, and the view constructor throws if they aren't
    const count = Math.floor(chunk.byteLength / 4);
    const out = Buffer.allocUnsafe(count * BYTES_PER_SAMPLE);
    let peak = 0;
    for (let index = 0; index < count; index++) {
      const sample = chunk.readFloatLE(index * 4);
      const magnitude = Math.abs(sample);
      if (magnitude > peak) peak = magnitude;
      // clamp before scaling: a tap mixing several processes can exceed 1.0 and
      // the wrap sounds like loud clicks
      const clamped = sample < -1 ? -1 : sample > 1 ? 1 : sample;
      out.writeInt16LE(Math.round(clamped * 32767), index * BYTES_PER_SAMPLE);
    }
    this.#dataBytes += out.byteLength;
    this.#stream.write(out);
    return peak;
  }

  async finish(): Promise<void> {
    this.#stream.end();
    await once(this.#stream, "close");
    const file = await open(this.path, "r+");
    try {
      await file.write(header(this.sampleRate, this.channels, this.#dataBytes), 0, HEADER_BYTES, 0);
    } finally {
      await file.close();
    }
  }
}
