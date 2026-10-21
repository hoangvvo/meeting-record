import type { AudioChunk } from "./index.js";

/**
 * Converts interleaved float32 at any rate and channel count into mono at
 * `outputRate`, carrying fractional position across chunks.
 * Downsampling averages each output's input span.
 */
export class MonoResampler {
  #outputRate: number;
  #sampleRate = 0;
  #step = 1;
  // Upsampling: the last input frame and the next output's input position,
  // relative to the current chunk (-1 is `#previous`).
  #previous = 0;
  #position = 0;
  // Downsampling: the weighted sum and input span gathered for the next output.
  #sum = 0;
  #covered = 0;

  constructor(outputRate: number) {
    this.#outputRate = outputRate;
  }

  resample(chunk: AudioChunk): Float32Array {
    if (chunk.sampleRate !== this.#sampleRate) {
      this.#sampleRate = chunk.sampleRate;
      this.#step = chunk.sampleRate / this.#outputRate;
      this.#previous = 0;
      this.#position = 0;
      this.#sum = 0;
      this.#covered = 0;
    }
    const input = downmix(chunk);
    if (this.#step === 1) return input;
    return this.#step > 1 ? this.#average(input) : this.#interpolate(input);
  }

  flush(): Float32Array {
    if (this.#step > 1) {
      const output =
        this.#covered > 0 ? Float32Array.of(this.#sum / this.#covered) : new Float32Array(0);
      this.#sum = 0;
      this.#covered = 0;
      return output;
    }
    const output = new Float32Array(Math.max(0, Math.ceil(-this.#position / this.#step - 1e-9)));
    output.fill(this.#previous);
    this.#position = 0;
    return output;
  }

  reset(): void {
    this.#sampleRate = 0;
    this.#sum = 0;
    this.#covered = 0;
    this.#position = 0;
    this.#previous = 0;
  }

  #average(input: Float32Array): Float32Array {
    const step = this.#step;
    const output = new Float32Array(Math.floor((this.#covered + input.length) / step) + 1);
    let written = 0;
    for (const sample of input) {
      let remaining = 1;
      while (remaining > 0) {
        const taken = Math.min(step - this.#covered, remaining);
        this.#sum += sample * taken;
        this.#covered += taken;
        remaining -= taken;
        if (this.#covered >= step - 1e-9) {
          output[written++] = this.#sum / step;
          this.#sum = 0;
          this.#covered = 0;
        }
      }
    }
    return output.subarray(0, written);
  }

  #interpolate(input: Float32Array): Float32Array {
    const last = input.length - 1;
    const output = new Float32Array(Math.max(0, Math.ceil((last - this.#position) / this.#step)));
    let written = 0;
    // Stops short of the final frame, which still needs its successor.
    while (this.#position < last && written < output.length) {
      const index = Math.floor(this.#position);
      const from = index < 0 ? this.#previous : input[index]!;
      const to = input[index + 1]!;
      output[written++] = from + (to - from) * (this.#position - index);
      this.#position += this.#step;
    }
    if (input.length > 0) {
      this.#position -= input.length;
      this.#previous = input[last]!;
    }
    return output.subarray(0, written);
  }
}

function downmix(chunk: AudioChunk): Float32Array {
  const samples = new Float32Array(
    chunk.pcm.buffer,
    chunk.pcm.byteOffset,
    chunk.pcm.byteLength / Float32Array.BYTES_PER_ELEMENT,
  );
  const { channels } = chunk;
  if (channels === 1) {
    return samples;
  }
  const mono = new Float32Array(Math.floor(samples.length / channels));
  for (let frame = 0; frame < mono.length; frame++) {
    let sum = 0;
    for (let channel = 0; channel < channels; channel++) {
      sum += samples[frame * channels + channel]!;
    }
    mono[frame] = sum / channels;
  }
  return mono;
}
