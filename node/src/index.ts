/**
 * meeting-record — system-audio and microphone capture with meeting detection.
 *
 * Wraps the native addon so callers never see helper-process pids, status codes,
 * or the realtime callback. The addon itself is not re-exported.
 */
import { EventEmitter } from "node:events";
import { Readable } from "node:stream";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

// Native addons cannot be imported under ESM. node-gyp-build selects the
// bundled binary for the current platform; local builds remain usable in-repo.
const loadNative = createRequire(import.meta.url)("node-gyp-build") as (root: string) => any;
const native = loadNative(fileURLToPath(new URL("..", import.meta.url)));

export type Permission = "system-audio" | "microphone" | "accessibility";
export type PermissionStatus = "granted" | "denied" | "unknown" | "not-required";

/**
 * Stable platform identifier; the values never change. No display labels are
 * provided — presentation and localisation belong to the caller.
 */
export type Platform =
  | "zoom"
  | "teams"
  | "meet"
  | "webex"
  | "slack"
  | "discord"
  /** A browser tab on a recognised call URL. */
  | "browser"
  | "unknown";

export interface Meeting {
  readonly platform: Platform;
  /** The visible app. Use it in `CaptureTarget` when starting capture. */
  readonly pid: number;
  readonly appName: string;
  /** Requires the accessibility permission; empty string otherwise. */
  readonly title: string;
  /** Requires the accessibility permission; empty string otherwise. */
  readonly url: string;
  readonly isUsingMic: boolean;
  readonly isPlayingAudio: boolean;
  readonly confidence: number;
  /** Prefer this to comparing `confidence`; the weights may change. */
  readonly shouldRecord: boolean;
}

export interface AudioProcess {
  readonly pid: number;
  readonly name: string;
  readonly bundleId: string;
  readonly isPlayingAudio: boolean;
  readonly isUsingMic: boolean;
}

export type CaptureTarget =
  | {
      /** A detected meeting's app pid, or any audio-producing process pid. */
      type: "process";
      pid: number;
    }
  | {
      /** All application output. */
      type: "system";
    };

export type MicrophoneSource = "default";

export interface CaptureOptions {
  /** Mono mixdown for system audio. Default true. */
  mono?: boolean;
  /** Capture the default microphone as a separate track. Default off. */
  microphone?: MicrophoneSource;
}

export class MeetingRecordError extends Error {
  constructor(
    message: string,
    readonly code: number,
  ) {
    super(message);
    this.name = new.target.name;
  }
}

export class UnsupportedOsError extends MeetingRecordError {}
export class PermissionError extends MeetingRecordError {}
export class AlreadyRunningError extends MeetingRecordError {}
export class CaptureError extends MeetingRecordError {}
export class InternalError extends MeetingRecordError {}

function errorFromStatus(message: string, code: number): MeetingRecordError {
  switch (code) {
    case -1:
      return new UnsupportedOsError(message, code);
    case -2:
      return new PermissionError(message, code);
    case -3:
      return new AlreadyRunningError(message, code);
    case -4:
    case -5:
    case -6:
    case -7:
    case -8:
      return new CaptureError(message, code);
    default:
      return new InternalError(message, code);
  }
}

function asLibraryError(error: unknown): Error {
  if (error instanceof TypeError) return error;
  const message = error instanceof Error ? error.message : String(error);
  const code = (error as { code?: unknown } | null)?.code;
  return typeof code === "number" ? errorFromStatus(message, code) : new InternalError(message, -9);
}

export const permissions = {
  status(permission: Permission): PermissionStatus {
    if (
      permission !== "system-audio" &&
      permission !== "microphone" &&
      permission !== "accessibility"
    ) {
      throw new TypeError(`unknown permission: ${String(permission)}`);
    }
    if (permission === "accessibility") return native.accessibilityPermissionStatus();
    if (permission === "microphone") return native.microphonePermissionStatus();
    return native.audioPermissionStatus();
  },

  /**
   * Show the permission prompt and resolve once the user answers.
   *
   * Accessibility opens System Settings and needs an app relaunch, so it resolves
   * as soon as Settings is open.
   */
  async request(
    permission: Permission,
    { timeoutMs = 120_000 }: { timeoutMs?: number } = {},
  ): Promise<PermissionStatus> {
    const current = permissions.status(permission);
    if (current === "granted" || current === "not-required") return current;

    if (permission === "accessibility") {
      const status = native.requestAccessibilityPermission();
      if (status !== 0) throw errorFromStatus("failed to request accessibility permission", status);
      return permissions.status(permission);
    }

    const requestStatus =
      permission === "microphone"
        ? native.requestMicrophonePermission()
        : native.requestAudioPermission();
    if (requestStatus !== 0) {
      throw errorFromStatus(`failed to request ${permission} permission`, requestStatus);
    }

    // The dialog is modal to the user, not to this process.
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 500));
      const status = permissions.status(permission);
      if (status === "granted" || status === "denied") return status;
    }
    return permissions.status(permission);
  },
};

function toMeeting(raw: any): Meeting {
  return raw as Meeting;
}

export interface MeetingEvents {
  started: (meeting: Meeting) => void;
  updated: (meeting: Meeting) => void;
  ended: (meeting: Meeting) => void;
}

class Meetings extends EventEmitter {
  /** Point-in-time scan, highest confidence first. */
  scan(): Meeting[] {
    return native.scan().map(toMeeting);
  }

  /** Begin emitting `started` / `updated` / `ended`. */
  watch(): void {
    if (native.isWatching()) return;
    const status = native.watchStart((event: keyof MeetingEvents, raw: any) => {
      this.emit(event, toMeeting(raw));
    });
    if (status !== 0) throw errorFromStatus("failed to start watching", status);
  }

  unwatch(): void {
    if (native.isWatching()) native.watchStop();
  }

  get watching(): boolean {
    return native.isWatching();
  }

  /** Every process doing audio IO. */
  audioProcesses(): AudioProcess[] {
    return native.listAudioProcesses();
  }
}

export const meetings = new Meetings() as Meetings & {
  on<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings;
  once<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings;
  off<E extends keyof MeetingEvents>(event: E, listener: MeetingEvents[E]): Meetings;
};

/**
 * Interleaved float32 PCM.
 *
 * The native callback never waits for a slow consumer. If this stream is not read,
 * its preallocated ring buffer drops samples and emits `drop`.
 */
export type CaptureState = "recording" | "paused" | "stopped";
export type CaptureHealth = "running" | "recovering" | "failed" | "stopped";

export interface AudioFormat {
  readonly sampleRate: number;
  readonly channels: number;
}

/**
 * A PCM buffer with the capture time of its first frame.
 *
 * `hostTimeNs` is monotonic (not Unix time) and uses the same host clock for the
 * system and microphone tracks, so callers can align or echo-cancel them without
 * relying on callback arrival order.
 */
export interface AudioChunk extends Buffer {
  readonly hostTimeNs: bigint;
  readonly frameCount: number;
  readonly sampleRate: number;
  readonly channels: number;
  readonly droppedSamples: number;
}

export interface AudioTrack extends Readable {
  /** Current negotiated format; a `format` event announces changes. */
  readonly sampleRate: number;
  readonly channels: number;
  /** Interleaved samples discarded because the bounded queue could not accept them. */
  readonly droppedSamples: number;
  on(event: "data", listener: (chunk: AudioChunk) => void): this;
  on(event: "drop", listener: (droppedSamples: number) => void): this;
  /** Emitted before the first chunk in a newly negotiated device format. */
  on(event: "format", listener: (format: AudioFormat) => void): this;
  on(event: string | symbol, listener: (...args: any[]) => void): this;
}

export interface CaptureSession extends EventEmitter {
  readonly systemAudio: AudioTrack;
  readonly microphone?: AudioTrack;
  readonly state: CaptureState;
  /** Native device health; independent of pause/resume state. */
  readonly health: CaptureHealth;
  /** Set when automatic recovery exhausts its retries. */
  readonly failure?: CaptureError;
  pauseRecording(): void;
  resumeRecording(): void;
  stopRecording(): Promise<void>;
  on(event: "interrupted", listener: () => void): this;
  on(event: "recovered", listener: () => void): this;
  on(event: "failed", listener: (error: CaptureError) => void): this;
  on(event: string | symbol, listener: (...args: any[]) => void): this;
}

class AudioTrackImpl extends Readable implements AudioTrack {
  #sampleRate: number;
  #channels: number;
  readonly #microphone: boolean;
  #droppedSamples = 0;
  #consumerReady = true;
  #finished = false;

  constructor(sampleRate: number, channels: number, microphone: boolean) {
    super({ objectMode: false, highWaterMark: 1 << 20 });
    this.#sampleRate = sampleRate;
    this.#channels = channels;
    this.#microphone = microphone;
  }

  /** Required by Readable; data arrives from the native side, not on demand. */
  override _read(): void {
    if (!this.#consumerReady && !this.#finished) {
      this.#consumerReady = true;
      native.captureSetConsumerReady(this.#microphone, true);
    }
  }

  get sampleRate(): number {
    return this.#sampleRate;
  }

  get channels(): number {
    return this.#channels;
  }

  get droppedSamples(): number {
    return this.#droppedSamples;
  }

  pushAudio(chunk: Buffer, info: NativeAudioInfo): void {
    if (info.dropped > 0) {
      this.#droppedSamples += info.dropped;
      this.emit("drop", info.dropped);
    }
    if (info.sampleRate !== this.#sampleRate || info.channels !== this.#channels) {
      this.#sampleRate = info.sampleRate;
      this.#channels = info.channels;
      this.emit("format", { sampleRate: info.sampleRate, channels: info.channels });
    }
    Object.defineProperties(chunk, {
      hostTimeNs: { value: info.hostTimeNs, enumerable: false },
      frameCount: { value: info.frameCount, enumerable: false },
      sampleRate: { value: info.sampleRate, enumerable: false },
      channels: { value: info.channels, enumerable: false },
      droppedSamples: { value: info.dropped, enumerable: false },
    });
    if (!this.push(chunk as AudioChunk) && this.#consumerReady) {
      this.#consumerReady = false;
      native.captureSetConsumerReady(this.#microphone, false);
    }
  }

  finish(): void {
    if (this.#finished) return;
    this.#finished = true;
    this.push(null);
  }
}

class CaptureSessionImpl extends EventEmitter implements CaptureSession {
  #state: CaptureState = "recording";
  #health: CaptureHealth = "running";
  #failure?: CaptureError;
  readonly #healthTimer: NodeJS.Timeout;
  readonly #systemAudio: AudioTrackImpl;
  readonly #microphone?: AudioTrackImpl;

  constructor(systemAudio: AudioTrackImpl, microphone?: AudioTrackImpl) {
    super();
    this.#systemAudio = systemAudio;
    this.#microphone = microphone;
    this.#healthTimer = setInterval(() => this.#pollHealth(), 250);
    this.#healthTimer.unref();
  }

  get systemAudio(): AudioTrackImpl {
    return this.#systemAudio;
  }

  get microphone(): AudioTrackImpl | undefined {
    return this.#microphone;
  }

  get state(): CaptureState {
    return this.#state;
  }

  get health(): CaptureHealth {
    return this.#health;
  }

  get failure(): CaptureError | undefined {
    return this.#failure;
  }

  pauseRecording(): void {
    if (this.#state === "stopped") return;
    if (this.#state !== "paused") {
      native.capturePause();
      this.#state = "paused";
    }
  }

  resumeRecording(): void {
    if (this.#state === "stopped") return;
    if (this.#state !== "recording") {
      native.captureResume();
      this.#state = "recording";
    }
  }

  async stopRecording(): Promise<void> {
    if (this.#state === "stopped") return;
    clearInterval(this.#healthTimer);
    native.captureStop();
    this.#state = "stopped";
    this.#health = "stopped";
    this.#finishTracks();
  }

  #finishTracks(): void {
    this.#systemAudio.finish();
    this.#microphone?.finish();
  }

  #pollHealth(): void {
    if (this.#state === "stopped") return;
    const health = native.captureHealthStatus() as CaptureHealth;
    if (health === this.#health) return;

    const previous = this.#health;
    this.#health = health;
    if (health === "recovering") {
      this.emit("interrupted");
    } else if (health === "running" && previous === "recovering") {
      this.emit("recovered");
    } else if (health === "failed") {
      const failure = new CaptureError(native.lastError(), -7);
      this.#failure = failure;
      clearInterval(this.#healthTimer);
      native.captureStop();
      this.#state = "stopped";
      this.#finishTracks();
      this.emit("failed", failure);
    }
  }
}

let captureStarting = false;

interface NativeAudioInfo {
  dropped: number;
  hostTimeNs: bigint;
  frameCount: number;
  sampleRate: number;
  channels: number;
}

export const capture = {
  async start(target: CaptureTarget, options: CaptureOptions = {}): Promise<CaptureSession> {
    if (captureStarting || native.isRunning()) {
      throw new AlreadyRunningError("capture already running", -3);
    }
    captureStarting = true;
    let session: CaptureSessionImpl | undefined;
    const pendingSystemAudio: Array<{ chunk: Buffer; info: NativeAudioInfo }> = [];
    const pendingMicrophone: Array<{ chunk: Buffer; info: NativeAudioInfo }> = [];

    const receive = (
      track: "systemAudio" | "microphone",
      pending: Array<{ chunk: Buffer; info: NativeAudioInfo }>,
      chunk: Buffer,
      info: NativeAudioInfo,
    ) => {
      if (!session) {
        pending.push({ chunk, info });
        return;
      }
      if (session.state !== "recording") return;
      session[track]?.pushAudio(chunk, info);
    };

    const onSystemAudio = (chunk: Buffer, info: NativeAudioInfo) => {
      receive("systemAudio", pendingSystemAudio, chunk, info);
    };
    const onMicrophone = (chunk: Buffer, info: NativeAudioInfo) => {
      receive("microphone", pendingMicrophone, chunk, info);
    };

    try {
      let nativeTarget: { pid: number } | { systemWide: true };
      if (target.type === "process") {
        if (!Number.isInteger(target.pid) || target.pid <= 0) {
          throw new TypeError("process pid must be a positive integer");
        }
        nativeTarget = { pid: target.pid };
      } else if (target.type === "system") {
        nativeTarget = { systemWide: true };
      } else {
        throw new TypeError(`unknown capture target: ${(target as any).type}`);
      }
      if (options.mono !== undefined && typeof options.mono !== "boolean") {
        throw new TypeError("mono must be a boolean");
      }
      if (options.microphone !== undefined && options.microphone !== "default") {
        throw new TypeError('microphone must be "default"');
      }
      const formats = await native.captureStart(
        {
          ...nativeTarget,
          mono: options.mono ?? true,
          microphone: options.microphone === "default",
        },
        onSystemAudio,
        onMicrophone,
      );
      const systemAudio = new AudioTrackImpl(
        formats.systemAudio.sampleRate,
        formats.systemAudio.channels,
        false,
      );
      const microphone = formats.microphone
        ? new AudioTrackImpl(formats.microphone.sampleRate, formats.microphone.channels, true)
        : undefined;
      session = new CaptureSessionImpl(systemAudio, microphone);
      for (const { chunk, info } of pendingSystemAudio) {
        systemAudio.pushAudio(chunk, info);
      }
      for (const { chunk, info } of pendingMicrophone) {
        microphone?.pushAudio(chunk, info);
      }
      return session;
    } catch (error) {
      throw asLibraryError(error);
    } finally {
      captureStarting = false;
    }
  },

  get running(): boolean {
    return captureStarting || native.captureHealthStatus() !== "stopped";
  },
};
