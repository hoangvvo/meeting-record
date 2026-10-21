import { Readable } from "node:stream";
import type { AudioChunk } from "./index.js";
import { getLevel, PcmMixer } from "./pcm-mixer.js";
import type { ReadableEvents, TypedReadable } from "./typed-readable.js";

export interface MixedAudioEvents extends ReadableEvents<Buffer> {
  level: (level: number) => void;
}

export interface MixedAudioOptions {
  sampleRate: number;
}

/** Mono signed 16-bit little-endian PCM. Await `end` to consume the final tail. */
export interface MixedAudio extends TypedReadable<MixedAudioEvents> {
  readonly sampleRate: number;
  readonly channels: 1;
  /** Affects the mixed output only; the raw microphone track stays available. */
  setMicrophoneMuted(muted: boolean): void;
}

export interface MixSource {
  setMixConsumer(consumer: ((chunk: AudioChunk) => void) | undefined): void;
  setMixReady(ready: boolean): void;
  resume(): unknown;
}

export class MixedAudioImpl extends Readable implements MixedAudio {
  readonly sampleRate: number;
  readonly channels = 1;
  readonly #sources: MixSource[];
  readonly #microphone: MixSource | undefined;
  readonly #mixer: PcmMixer;
  readonly #timer: NodeJS.Timeout;
  #startup: Map<MixSource, AudioChunk[]> | undefined = new Map();
  #startupDeadline = 0;
  #startupTimer?: NodeJS.Timeout;
  #blocked = false;
  #pumping = false;
  #paused = false;
  #finished = false;

  constructor(sampleRate: number, systemAudio: MixSource, microphone?: MixSource) {
    super({ highWaterMark: sampleRate * 2 });
    this.sampleRate = sampleRate;
    this.#sources = microphone ? [systemAudio, microphone] : [systemAudio];
    this.#microphone = microphone;
    this.#mixer = new PcmMixer(sampleRate, this.#sources);
    for (const source of this.#sources) {
      source.setMixConsumer((chunk) => this.#write(source, chunk));
      source.resume();
    }
    this.#timer = setInterval(() => this.#pump(), 100);
    this.#timer.unref();
  }

  override _read(): void {
    this.#blocked = false;
    this.#pump();
    if (!this.#blocked && !this.#finished) {
      for (const source of this.#sources) source.setMixReady(true);
    }
  }

  override _destroy(error: Error | null, callback: (error?: Error | null) => void): void {
    clearInterval(this.#timer);
    clearTimeout(this.#startupTimer);
    this.#startup?.clear();
    for (const source of this.#sources) {
      source.setMixConsumer(undefined);
      source.setMixReady(true);
    }
    callback(error);
  }

  setMicrophoneMuted(muted: boolean): void {
    if (this.#microphone) this.#mixer.setMuted(this.#microphone, muted);
  }

  pauseCapture(): void {
    if (this.#paused || this.#finished || this.destroyed) return;
    this.#paused = true;
    this.#flushStartup(true);
    this.#mixer.finishSegment();
    this.#pump();
  }

  resumeCapture(): void {
    if (!this.#paused || this.#finished || this.destroyed) return;
    this.#paused = false;
    this.#mixer.restart();
    this.#startup = new Map();
  }

  finishCapture(): void {
    if (this.#finished || this.destroyed) return;
    clearInterval(this.#timer);
    this.#finished = true;
    this.#flushStartup(true);
    this.#mixer.finishSegment();
    this.#pump();
  }

  #write(source: MixSource, chunk: AudioChunk): void {
    if (!this.#startup) {
      this.#mixer.write(source, chunk);
      return;
    }
    if (this.#startup.size === 0) {
      this.#startupDeadline = performance.now() + 100;
      this.#startupTimer = setTimeout(() => {
        this.#flushStartup(true);
        this.#pump();
      }, 100);
      this.#startupTimer.unref();
    }
    const pending = this.#startup.get(source) ?? [];
    pending.push(chunk);
    this.#startup.set(source, pending);
    this.#flushStartup();
  }

  #flushStartup(force = false): void {
    if (!this.#startup?.size) return;
    if (
      !force &&
      this.#startup.size < this.#sources.length &&
      performance.now() < this.#startupDeadline
    ) {
      return;
    }
    clearTimeout(this.#startupTimer);
    // Establish the earliest source timestamp while retaining each source's order.
    const pending = [...this.#startup].sort(([, left], [, right]) => {
      if (left[0]!.hostTimeNs < right[0]!.hostTimeNs) return -1;
      return left[0]!.hostTimeNs > right[0]!.hostTimeNs ? 1 : 0;
    });
    this.#startup = undefined;
    for (const [source, chunks] of pending) {
      for (const chunk of chunks) this.#mixer.write(source, chunk);
    }
  }

  #pump(): void {
    if (this.#pumping || this.destroyed) return;
    this.#pumping = true;
    this.#flushStartup();
    while (!this.#blocked) {
      const chunk = this.#mixer.read();
      if (!chunk) {
        if (this.#finished) this.push(null);
        break;
      }
      if (!this.push(chunk)) {
        this.#blocked = true;
        for (const source of this.#sources) source.setMixReady(false);
      }
      this.emit("level", getLevel(chunk));
    }
    this.#pumping = false;
  }
}
