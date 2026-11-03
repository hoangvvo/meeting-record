/** System-audio and microphone capture with meeting detection. */
import { EventEmitter } from "node:events";
import { Readable } from "node:stream";
import { setTimeout as delay } from "node:timers/promises";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

import {
  MixedAudioImpl,
  type MixedAudio,
  type MixedAudioOptions,
  type NativePcmMixer,
} from "./mixed-audio.js";
import type { EmitterEvents, ReadableEvents, TypedReadable } from "./typed-readable.js";
export type { MixedAudio, MixedAudioEvents, MixedAudioOptions } from "./mixed-audio.js";

export const Permission = {
  SystemAudio: "system-audio",
  Microphone: "microphone",
  Accessibility: "accessibility",
} as const;
export type Permission = (typeof Permission)[keyof typeof Permission];

/**
 * Windows has no prompts: system audio and accessibility are always granted, and
 * the microphone follows its privacy settings.
 */
export const PermissionStatus = {
  Granted: "granted",
  Denied: "denied",
  Unknown: "unknown",
} as const;
export type PermissionStatus = (typeof PermissionStatus)[keyof typeof PermissionStatus];

/** Stable platform identifier. */
export const Platform = {
  Zoom: "zoom",
  Teams: "teams",
  Meet: "meet",
  Webex: "webex",
  Slack: "slack",
  Discord: "discord",
  /** A browser tab on a recognised call URL. */
  Browser: "browser",
  GoTo: "goto",
  Dialpad: "dialpad",
  WhatsApp: "whatsapp",
  Aircall: "aircall",
  RingCentral: "ringcentral",
  EightByEight: "8x8",
  VooV: "voov",
  Tuple: "tuple",
  Gather: "gather",
  Lark: "lark",
  Roam: "roam",
  ClickUp: "clickup",
  Telemost: "telemost",
  Telegram: "telegram",
  Signal: "signal",
  Viber: "viber",
  Line: "line",
  WeChat: "wechat",
  Zalo: "zalo",
  Jitsi: "jitsi",
  Element: "element",
  Mattermost: "mattermost",
  NextcloudTalk: "nextcloud-talk",
  Jami: "jami",
  TencentMeeting: "tencent-meeting",
  WeCom: "wecom",
  Unknown: "unknown",
} as const;
export type Platform = (typeof Platform)[keyof typeof Platform];

export type MeetingEvent = "started" | "updated" | "ended";

export interface Meeting {
  readonly platform: Platform;
  /** The visible app. Use it in `CaptureTarget` when starting capture. */
  readonly pid: number;
  readonly appName: string;
  /** Window or tab title. Requires the accessibility permission on macOS; empty string otherwise. */
  readonly title: string;
  /**
   * The active tab's address for browser meetings. Requires the accessibility permission on
   * macOS; empty string otherwise, or when the call is in a background tab.
   */
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
      /** All application output except the recorder's process tree. */
      type: "system";
    };

export const MicrophoneSource = {
  /** Follow the OS default input device. */
  Default: "default",
  /** Prefer the captured app's mic, then the default (communications default on Windows). Ambiguous app inputs use the default on macOS. */
  Auto: "auto",
} as const;
export type MicrophoneSource = (typeof MicrophoneSource)[keyof typeof MicrophoneSource];

/** Optional microphone effects for input rates from 8 to 192 kHz. All default off. */
export interface MicrophoneProcessing {
  /** Cancel echo using the captured system track as the playback reference. */
  echoCancellation?: boolean;
  /** Suppress background noise in the microphone track. */
  noiseSuppression?: boolean;
  /** Adjust microphone gain automatically. */
  autoGainControl?: boolean;
}

export interface CaptureOptions {
  /** Mono mixdown for system audio only; the microphone keeps its format. Default true. */
  mono?: boolean;
  /** Capture a separate microphone track. Default off; unavailable devices recover while system capture continues. */
  microphone?: MicrophoneSource;
  /** Optional microphone effects. All default off; requires microphone. */
  microphoneProcessing?: MicrophoneProcessing;
  /** Additional mono s16le output, aligned and resampled to the requested rate. */
  mix?: MixedAudioOptions;
  /** Aborting stops capture, including a pending start. */
  signal?: AbortSignal;
}

/** Base class for library errors with a native status code. */
export class MeetingRecordError extends Error {
  constructor(
    message: string,
    /** Native status code. */
    readonly code: number,
  ) {
    super(message);
    this.name = new.target.name;
  }
}

/** The OS, architecture, or OS version is unsupported. */
export class UnsupportedOsError extends MeetingRecordError {}
/** Required system-audio or microphone access has not been granted. */
export class PermissionError extends MeetingRecordError {}
/** A process-wide recording or watcher is already active. */
export class AlreadyRunningError extends MeetingRecordError {}
/** An audio capture operation failed. */
export class CaptureError extends MeetingRecordError {}
/** Native initialization or another internal operation failed. */
export class InternalError extends MeetingRecordError {}

interface NativeFormat {
  sampleRate: number;
  channels: number;
  deviceName?: string;
}
interface NativeBinding {
  PcmMixer: new (sampleRate: number, sourceCount: number) => NativePcmMixer;
  audioPermissionStatus(): PermissionStatus;
  microphonePermissionStatus(): PermissionStatus;
  accessibilityPermissionStatus(): PermissionStatus;
  requestAudioPermission(): number;
  requestMicrophonePermission(): number;
  requestAccessibilityPermission(): number;
  scan(): Promise<Meeting[]>;
  listAudioProcesses(): AudioProcess[];
  listAudioApps(): AudioProcess[];
  watchStart(callback: (event: MeetingEvent, meeting: Meeting) => void): number;
  watchStop(): number;
  isWatching(): boolean;
  isRunning(): boolean;
  captureStart(
    options: { pid?: number; systemWide?: true; mono: boolean; microphone: number },
    systemAudio: (pcm: Buffer, info: NativeAudioInfo) => void,
    microphone: (pcm: Buffer, info: NativeAudioInfo) => void,
    event: (event: NativeTrackEvent) => void,
  ): Promise<{ systemAudio: NativeFormat; microphone?: NativeFormat }>;
  captureStop(): Promise<void>;
  capturePause(): void;
  captureResume(): void;
  captureSetConsumerReady(microphone: boolean, ready: boolean): void;
}
let cachedNative: NativeBinding | undefined;

function getNative(): NativeBinding {
  if (cachedNative) return cachedNative;
  if (
    !["darwin", "win32", "linux"].includes(process.platform) ||
    (process.arch !== "arm64" && process.arch !== "x64")
  ) {
    throw new UnsupportedOsError(
      `meeting-record native APIs require macOS, Windows, or Linux on arm64 or x64; received ${process.platform}-${process.arch}`,
      -1,
    );
  }

  // Defer loading so other platforms can install and import the JavaScript API.
  // node-gyp-build selects a bundled binary or a local development build.
  const loadNative = createRequire(import.meta.url)("node-gyp-build") as (
    root: string,
  ) => NativeBinding;
  cachedNative = loadNative(fileURLToPath(new URL("..", import.meta.url)));
  return cachedNative;
}

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
  /**
   * Check the current permission without prompting.
   * @throws {TypeError} If the permission is unknown.
   * @throws {UnsupportedOsError} If the platform is unsupported.
   */
  status(permission: Permission): PermissionStatus {
    if (
      permission !== Permission.SystemAudio &&
      permission !== Permission.Microphone &&
      permission !== Permission.Accessibility
    ) {
      throw new TypeError(`unknown permission: ${String(permission)}`);
    }
    const native = getNative();
    if (permission === Permission.Accessibility) return native.accessibilityPermissionStatus();
    if (permission === Permission.Microphone) return native.microphonePermissionStatus();
    return native.audioPermissionStatus();
  },

  /**
   * Show the permission prompt and resolve once the user answers.
   *
   * Accessibility opens System Settings and needs an app relaunch, so it resolves
   * as soon as Settings is open.
   * Returns the current status if the timeout expires.
   * @throws {TypeError} If the permission is unknown.
   * @throws {MeetingRecordError} If the platform cannot start the permission request.
   * @throws If the request is aborted.
   */
  async request(
    permission: Permission,
    { timeoutMs = 120_000, signal }: { timeoutMs?: number; signal?: AbortSignal } = {},
  ): Promise<PermissionStatus> {
    signal?.throwIfAborted();
    const current = permissions.status(permission);
    if (current === PermissionStatus.Granted) return current;

    const native = getNative();
    if (permission === Permission.Accessibility) {
      const status = native.requestAccessibilityPermission();
      if (status !== 0) throw errorFromStatus("failed to request accessibility permission", status);
      return permissions.status(permission);
    }

    const requestStatus =
      permission === Permission.Microphone
        ? native.requestMicrophonePermission()
        : native.requestAudioPermission();
    if (requestStatus !== 0) {
      throw errorFromStatus(`failed to request ${permission} permission`, requestStatus);
    }

    // The dialog is modal to the user, not to this process.
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await delay(500, undefined, { signal });
      const status = permissions.status(permission);
      if (status === PermissionStatus.Granted || status === PermissionStatus.Denied) return status;
    }
    return permissions.status(permission);
  },
};

export interface MeetingEvents extends EmitterEvents {
  started: (meeting: Meeting) => void;
  updated: (meeting: Meeting) => void;
  ended: (meeting: Meeting) => void;
}

class Meetings extends EventEmitter<{
  [Event in keyof MeetingEvents]: Parameters<MeetingEvents[Event]>;
}> {
  /**
   * Point-in-time scan, highest confidence first. Use `shouldRecord` to select calls.
   * @throws {MeetingRecordError} If the platform or audio service cannot be queried.
   */
  async scan(): Promise<Meeting[]> {
    try {
      return await getNative().scan();
    } catch (error) {
      throw asLibraryError(error);
    }
  }

  /**
   * Begin emitting `started` / `updated` / `ended`. Repeated calls are harmless.
   * @throws {MeetingRecordError} If the watcher cannot start.
   */
  watch(): void {
    const native = getNative();
    if (native.isWatching()) return;
    const status = native.watchStart((event, meeting) => {
      this.emit(event, meeting);
    });
    if (status !== 0) throw errorFromStatus("failed to start watching", status);
  }

  /** Stop meeting events; safe to call when already stopped. */
  unwatch(): void {
    const native = getNative();
    if (native.isWatching()) native.watchStop();
  }

  /** Whether meeting events are currently being watched. */
  get watching(): boolean {
    return getNative().isWatching();
  }

  /**
   * Every process doing audio IO.
   * @throws {MeetingRecordError} If audio processes cannot be enumerated.
   */
  audioProcesses(): AudioProcess[] {
    try {
      return getNative().listAudioProcesses();
    } catch (error) {
      throw asLibraryError(error);
    }
  }

  /**
   * Applications doing audio IO, grouped by owning PID, including helpers.
   * Pass `pid` as a process target; no meeting scan or Accessibility grant is needed.
   * @throws {MeetingRecordError} If audio applications cannot be enumerated.
   */
  audioApps(): AudioProcess[] {
    try {
      return getNative().listAudioApps();
    } catch (error) {
      throw asLibraryError(error);
    }
  }
}

export const meetings = new Meetings();

export const CaptureState = {
  Recording: "recording",
  Paused: "paused",
  Stopped: "stopped",
} as const;
export type CaptureState = (typeof CaptureState)[keyof typeof CaptureState];

/** Reason for source loss. */
export const TrackInterruptionReason = {
  DeviceRemoved: "device-removed",
  DeviceChanged: "device-changed",
  FormatChanged: "format-changed",
  AppExited: "app-exited",
  ServiceReset: "service-reset",
  Unknown: "unknown",
} as const;
export type TrackInterruptionReason =
  (typeof TrackInterruptionReason)[keyof typeof TrackInterruptionReason];

export interface AudioFormat {
  /** May be zero while the requested microphone has no available device. */
  readonly sampleRate: number;
  readonly channels: number;
}

/** PCM packet timed on a monotonic clock shared by both tracks. */
export interface AudioChunk {
  /** Interleaved float32 PCM. Packet boundaries and metadata survive iteration. */
  readonly pcm: Buffer;
  /** First frame's monotonic timestamp in nanoseconds, shared across tracks. */
  readonly hostTimeNs: bigint;
  /** Number of frames per channel in this packet. */
  readonly frameCount: number;
  /** This packet's rate; it may change after device recovery. */
  readonly sampleRate: number;
  readonly channels: number;
  /** Interleaved samples lost before this packet because the input queue was full. */
  readonly droppedSamples: number;
}

export const TrackAvailability = {
  Live: "live",
  Degraded: "degraded",
  Interrupted: "interrupted",
} as const;
export type TrackAvailability = (typeof TrackAvailability)[keyof typeof TrackAvailability];

export interface AudioTrackEvents extends ReadableEvents<AudioChunk> {
  drop: (droppedSamples: number) => void;
  /** Emitted before the first chunk in a newly negotiated device format. */
  format: (format: AudioFormat) => void;
  /** No source is available. Lost devices are retried; exited apps require a new target. */
  interrupted: (reason: TrackInterruptionReason, message: string) => void;
  resumed: () => void;
  /** Some sources are lost; remaining sources continue delivering audio. */
  degraded: (reason: TrackInterruptionReason, message: string) => void;
  /** The microphone moved to another device. */
  devicechange: (deviceName: string) => void;
}

/** Interleaved float32 PCM. Slow consumers lose samples, reported by `drop`. */
export interface AudioTrack extends TypedReadable<AudioTrackEvents> {
  readonly availability: TrackAvailability;
  [Symbol.asyncIterator](): NodeJS.AsyncIterator<AudioChunk>;
  /** Current negotiated format; a `format` event announces changes. */
  readonly sampleRate: number;
  readonly channels: number;
  /** Interleaved samples discarded because the bounded queue could not accept them. */
  readonly droppedSamples: number;
  /** True when none of this track's sources are available. */
  readonly interrupted: boolean;
  /** The microphone in use; undefined for system audio. */
  readonly deviceName?: string;
}

/** A lost source interrupts its track; the session records until stopped. */
export interface CaptureSession {
  readonly systemAudio: AudioTrack;
  readonly microphone?: AudioTrack;
  readonly mixedAudio?: MixedAudio;
  readonly state: CaptureState;
  /** Pause all tracks. Paused time is omitted from mixed output. */
  pauseRecording(): void;
  /** Resume all tracks after a pause. */
  resumeRecording(): void;
  /**
   * Stop once, then read outputs until `end` to drain their final audio.
   * Repeated calls share the same promise.
   * @throws {Error} If native capture teardown fails.
   */
  stopRecording(): Promise<void>;
}

export interface MixedCaptureSession extends CaptureSession {
  readonly mixedAudio: MixedAudio;
}

class AudioTrackImpl extends Readable implements AudioTrack {
  #sampleRate: number;
  #channels: number;
  readonly #microphone: boolean;
  #droppedSamples = 0;
  #consumerReady = true;
  #mixReady = true;
  #mixConsumer: ((chunk: AudioChunk) => void) | undefined;
  #finished = false;
  #availability: TrackAvailability = TrackAvailability.Live;
  #deviceName?: string;

  constructor(sampleRate: number, channels: number, microphone: boolean, deviceName?: string) {
    super({ objectMode: true, highWaterMark: 32 });
    this.#sampleRate = sampleRate;
    this.#channels = channels;
    this.#microphone = microphone;
    this.#deviceName = deviceName;
  }

  /** Required by Readable; data arrives from the native side, not on demand. */
  override _read(): void {
    if (!this.#consumerReady && !this.#finished) {
      this.#consumerReady = true;
      getNative().captureSetConsumerReady(this.#microphone, this.#mixReady);
    }
  }

  setMixConsumer(consumer: ((chunk: AudioChunk) => void) | undefined): void {
    this.#mixConsumer = consumer;
  }

  setMixReady(ready: boolean): void {
    this.#mixReady = ready;
    if (!this.#finished)
      getNative().captureSetConsumerReady(this.#microphone, ready && this.#consumerReady);
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

  get availability(): TrackAvailability {
    return this.#availability;
  }

  override [Symbol.asyncIterator](): NodeJS.AsyncIterator<AudioChunk> {
    return super[Symbol.asyncIterator]();
  }

  get interrupted(): boolean {
    return this.#availability === TrackAvailability.Interrupted;
  }

  get deviceName(): string | undefined {
    return this.#deviceName;
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
    const packet: AudioChunk = {
      pcm: chunk,
      hostTimeNs: info.hostTimeNs,
      frameCount: info.frameCount,
      sampleRate: info.sampleRate,
      channels: info.channels,
      droppedSamples: info.dropped,
    };
    this.#mixConsumer?.(packet);
    if (!this.push(packet) && this.#consumerReady) {
      this.#consumerReady = false;
      getNative().captureSetConsumerReady(this.#microphone, false);
    }
  }

  pushEvent(event: NativeTrackEvent): void {
    if (this.#finished) return;
    if (event.type === "interrupted") {
      this.#availability = TrackAvailability.Interrupted;
      this.emit("interrupted", event.reason, event.message);
    } else if (event.type === "resumed") {
      this.#availability = TrackAvailability.Live;
      this.emit("resumed");
    } else if (event.type === "degraded") {
      this.#availability = TrackAvailability.Degraded;
      this.emit("degraded", event.reason, event.message);
    } else {
      this.#deviceName = event.deviceName;
      this.emit("devicechange", event.deviceName);
    }
  }

  finish(): void {
    if (this.#finished) return;
    this.#finished = true;
    this.push(null);
  }
}

class CaptureSessionImpl implements CaptureSession {
  #state: CaptureState = CaptureState.Recording;
  #stopping?: Promise<void>;
  readonly #systemAudio: AudioTrackImpl;
  readonly #microphone?: AudioTrackImpl;
  readonly mixedAudio?: MixedAudioImpl;
  readonly #signal?: AbortSignal;
  readonly #onAbort = () => {
    void this.stopRecording();
  };

  constructor(
    systemAudio: AudioTrackImpl,
    microphone?: AudioTrackImpl,
    mix?: MixedAudioOptions,
    signal?: AbortSignal,
  ) {
    this.#systemAudio = systemAudio;
    this.#microphone = microphone;
    this.#signal = signal;
    signal?.addEventListener("abort", this.#onAbort, { once: true });
    this.mixedAudio = mix
      ? new MixedAudioImpl(
          mix.sampleRate,
          systemAudio,
          microphone,
          new (getNative().PcmMixer)(mix.sampleRate, microphone ? 2 : 1),
        )
      : undefined;
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

  pauseRecording(): void {
    if (this.#state === CaptureState.Stopped || this.#stopping) return;
    if (this.#state !== CaptureState.Paused) {
      getNative().capturePause();
      this.#state = CaptureState.Paused;
      this.mixedAudio?.pauseCapture();
    }
  }

  resumeRecording(): void {
    if (this.#state === CaptureState.Stopped || this.#stopping) return;
    if (this.#state !== CaptureState.Recording) {
      this.mixedAudio?.resumeCapture();
      getNative().captureResume();
      this.#state = CaptureState.Recording;
    }
  }

  stopRecording(): Promise<void> {
    if (this.#stopping) return this.#stopping;
    if (this.#state === CaptureState.Stopped) return Promise.resolve();
    this.#stopping = this.#stop();
    return this.#stopping;
  }

  async #stop(): Promise<void> {
    try {
      await getNative().captureStop();
    } finally {
      this.#state = CaptureState.Stopped;
      this.#signal?.removeEventListener("abort", this.#onAbort);
      this.#systemAudio.finish();
      this.#microphone?.finish();
      this.mixedAudio?.finishCapture();
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

interface NativeTrackEvent {
  track: "systemAudio" | "microphone";
  type: "interrupted" | "resumed" | "devicechange" | "degraded";
  reason: TrackInterruptionReason;
  deviceName: string;
  message: string;
}

/**
 * Start a process-wide recording. Attach source-event listeners after awaiting startup.
 * @throws {TypeError} If the target or capture options are invalid.
 * @throws {AlreadyRunningError} If another recording is active or starting.
 * @throws {PermissionError} If required capture permissions are missing.
 * @throws {CaptureError} If the target or system audio cannot be opened.
 * @throws {UnsupportedOsError} If the platform is unsupported.
 * @throws {InternalError} If native initialization fails.
 * @throws The signal's reason if startup is aborted.
 */
function startCapture(
  target: CaptureTarget,
  options: CaptureOptions & { mix: MixedAudioOptions },
): Promise<MixedCaptureSession>;
/**
 * Start a process-wide recording. Attach source-event listeners after awaiting startup.
 * @throws {TypeError} If the target or capture options are invalid.
 * @throws {AlreadyRunningError} If another recording is active or starting.
 * @throws {PermissionError} If required capture permissions are missing.
 * @throws {CaptureError} If the target or system audio cannot be opened.
 * @throws {UnsupportedOsError} If the platform is unsupported.
 * @throws {InternalError} If native initialization fails.
 * @throws The signal's reason if startup is aborted.
 */
function startCapture(target: CaptureTarget, options?: CaptureOptions): Promise<CaptureSession>;
async function startCapture(
  target: CaptureTarget,
  options: CaptureOptions = {},
): Promise<CaptureSession> {
  options.signal?.throwIfAborted();
  let nativeTarget: { pid: number } | { systemWide: true };
  if (target.type === "process") {
    if (!Number.isInteger(target.pid) || target.pid <= 0 || target.pid > 0x7fffffff) {
      throw new TypeError("process pid must be an integer from 1 through 2147483647");
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
  if (
    options.microphone !== undefined &&
    options.microphone !== MicrophoneSource.Default &&
    options.microphone !== MicrophoneSource.Auto
  ) {
    throw new TypeError('microphone must be "default" or "auto"');
  }
  if (
    options.mix !== undefined &&
    (!Number.isInteger(options.mix.sampleRate) || options.mix.sampleRate <= 0)
  ) {
    throw new TypeError("mix sampleRate must be a positive integer");
  }
  let processing = 0;
  if (options.microphoneProcessing !== undefined) {
    const settings = options.microphoneProcessing;
    if (!settings || typeof settings !== "object")
      throw new TypeError("microphoneProcessing must be an object");
    for (const [key, bit] of [
      ["echoCancellation", 0x100],
      ["noiseSuppression", 0x200],
      ["autoGainControl", 0x400],
    ] as const) {
      if (settings[key] !== undefined && typeof settings[key] !== "boolean")
        throw new TypeError(`${key} must be a boolean`);
      if (settings[key]) processing |= bit;
    }
    if (processing && !options.microphone)
      throw new TypeError("microphoneProcessing requires microphone");
  }

  const native = getNative();
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
    if (session.state !== CaptureState.Recording) return;
    session[track]?.pushAudio(chunk, info);
  };

  const onSystemAudio = (chunk: Buffer, info: NativeAudioInfo) => {
    receive("systemAudio", pendingSystemAudio, chunk, info);
  };
  const onMicrophone = (chunk: Buffer, info: NativeAudioInfo) => {
    receive("microphone", pendingMicrophone, chunk, info);
  };
  const pendingEvents: NativeTrackEvent[] = [];
  let eventsReady = false;
  const onTrackEvent = (event: NativeTrackEvent) => {
    if (!session || !eventsReady) {
      pendingEvents.push(event);
      return;
    }
    session[event.track]?.pushEvent(event);
  };

  try {
    const formats = await native.captureStart(
      {
        ...nativeTarget,
        mono: options.mono ?? true,
        microphone:
          processing |
          (options.microphone === MicrophoneSource.Auto
            ? 2
            : options.microphone === MicrophoneSource.Default
              ? 1
              : 0),
      },
      onSystemAudio,
      onMicrophone,
      onTrackEvent,
    );
    if (options.signal?.aborted) {
      await native.captureStop();
      options.signal.throwIfAborted();
    }
    const systemAudio = new AudioTrackImpl(
      formats.systemAudio.sampleRate,
      formats.systemAudio.channels,
      false,
    );
    const microphone = formats.microphone
      ? new AudioTrackImpl(
          formats.microphone.sampleRate,
          formats.microphone.channels,
          true,
          formats.microphone.deviceName || undefined,
        )
      : undefined;
    const recording = new CaptureSessionImpl(systemAudio, microphone, options.mix, options.signal);
    session = recording;
    for (const { chunk, info } of pendingSystemAudio) {
      systemAudio.pushAudio(chunk, info);
    }
    for (const { chunk, info } of pendingMicrophone) {
      microphone?.pushAudio(chunk, info);
    }
    pendingSystemAudio.length = 0;
    pendingMicrophone.length = 0;
    setImmediate(() => {
      for (const event of pendingEvents) recording[event.track]?.pushEvent(event);
      pendingEvents.length = 0;
      eventsReady = true;
    });
    return recording;
  } catch (error) {
    if (options.signal?.aborted) throw error;
    throw asLibraryError(error);
  } finally {
    captureStarting = false;
  }
}

export const capture = {
  start: startCapture,
  /** Whether a recording is starting or active, including while paused. */
  get running(): boolean {
    return captureStarting || getNative().isRunning();
  },
};
