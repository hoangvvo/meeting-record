import { Readable, Writable } from "node:stream";
import { pipeline } from "node:stream/promises";
import {
  capture,
  meetings,
  permissions,
  CaptureState,
  MicrophoneSource,
  Permission,
  PermissionStatus,
  Platform,
  TrackAvailability,
  TrackInterruptionReason,
  type AudioChunk,
  type AudioFormat,
  type AudioTrack,
  type CaptureSession,
  type CaptureInfo,
  type CaptureTarget,
  type Meeting,
  type MeetingEvent,
  type MixedAudio,
  type MicrophoneProcessing,
} from "meeting-record";

declare function expectType<Type>(value: Type): void;
declare const meeting: Meeting;
declare const track: AudioTrack;
declare const mixed: MixedAudio;
declare const session: CaptureSession;
declare const destination: Writable;
track.on("capturechange", (info) => {
  expectType<CaptureInfo>(info);
  expectType<Readonly<CaptureTarget>>(info.actualTarget);
  if (info.actualTarget.type === "process") {
    expectType<number>(info.actualTarget.pid);
  } else {
    // @ts-expect-error System targets have no PID.
    expectType<unknown>(info.actualTarget.pid);
  }
  expectType<boolean>(info.volumeDependent);
  // @ts-expect-error Capture state does not expose a guessed stream category.
  expectType<unknown>(info.communicationsCategory);
});
expectType<CaptureInfo | undefined>(track.captureInfo);
capture.start({ type: "process", pid: 123 }, { allowSystemAudioFallback: true });
// @ts-expect-error Fallback selection must be a boolean.
capture.start({ type: "system" }, { allowSystemAudioFallback: "auto" });

// Value exports share their names with the corresponding union types.
expectType<Platform>(Platform.Zoom);
expectType<Permission>(Permission.SystemAudio);
expectType<PermissionStatus>(PermissionStatus.Granted);
expectType<MicrophoneSource>(MicrophoneSource.Auto);
expectType<MicrophoneProcessing>({
  echoCancellation: true,
  noiseSuppression: false,
  autoGainControl: true,
});
// @ts-expect-error Effect settings are boolean flags.
expectType<MicrophoneProcessing>({ noiseSuppression: "yes" });
expectType<CaptureState>(CaptureState.Recording);
expectType<TrackAvailability>(TrackAvailability.Live);
expectType<TrackInterruptionReason>(TrackInterruptionReason.DeviceRemoved);
expectType<"zoom">(Platform.Zoom);
expectType<"goto">(Platform.GoTo);
expectType<"dialpad">(Platform.Dialpad);
expectType<"whatsapp">(Platform.WhatsApp);
expectType<"aircall">(Platform.Aircall);
expectType<"ringcentral">(Platform.RingCentral);
expectType<"8x8">(Platform.EightByEight);
expectType<"voov">(Platform.VooV);
expectType<"tuple">(Platform.Tuple);
expectType<"gather">(Platform.Gather);
expectType<"lark">(Platform.Lark);
expectType<"roam">(Platform.Roam);
expectType<"clickup">(Platform.ClickUp);
expectType<"telemost">(Platform.Telemost);
expectType<"telegram">(Platform.Telegram);
expectType<"signal">(Platform.Signal);
expectType<"viber">(Platform.Viber);
expectType<"line">(Platform.Line);
expectType<"wechat">(Platform.WeChat);
expectType<"zalo">(Platform.Zalo);
expectType<"jitsi">(Platform.Jitsi);
expectType<"element">(Platform.Element);
expectType<"mattermost">(Platform.Mattermost);
expectType<"nextcloud-talk">(Platform.NextcloudTalk);
expectType<"jami">(Platform.Jami);

expectType<"tencent-meeting">(Platform.TencentMeeting);
expectType<"wecom">(Platform.WeCom);
expectType<MeetingEvent>("started");
expectType<Platform>(meeting.platform);
expectType<CaptureState>(session.state);
expectType<TrackAvailability>(track.availability);
expectType<PermissionStatus>(permissions.status(Permission.Microphone));

const target: CaptureTarget = { type: "process", pid: meeting.pid };
capture.start(target, { microphone: MicrophoneSource.Default });
capture.start({ type: "system" }, { mix: { sampleRate: 24_000 } }).then((recording) => {
  expectType<MixedAudio>(recording.mixedAudio);
});

meetings
  .on("started", (value) => {
    expectType<Meeting>(value);
    // @ts-expect-error Meeting callbacks must not infer any.
    expectType<unknown>(value.nonexistentField);
  })
  .once("updated", (value) => expectType<Meeting>(value))
  .off("ended", (value) => expectType<Meeting>(value))
  .prependListener("started", (value) => expectType<Meeting>(value))
  .prependOnceListener("updated", (value) => expectType<Meeting>(value))
  .addListener("ended", (value) => expectType<Meeting>(value))
  .removeListener("ended", (value) => expectType<Meeting>(value));

// @ts-expect-error Misspelled event names must not use EventEmitter's fallback.
meetings.on("strated", () => {});
// @ts-expect-error Incorrect callback payloads must be rejected.
meetings.on("started", (_value: number) => {});
// @ts-expect-error Chaining must preserve the typed emitter.
meetings.on("started", () => {}).once("updatde", () => {});
// @ts-expect-error emit also checks meeting payloads.
meetings.emit("started", 123);

track
  .on("data", (chunk) => {
    expectType<AudioChunk>(chunk);
    // @ts-expect-error Raw audio carries packet metadata, not a bare Buffer.
    expectType<Buffer>(chunk);
  })
  .once("format", (format) => expectType<AudioFormat>(format))
  .on("interrupted", (reason, message) => {
    expectType<TrackInterruptionReason>(reason);
    expectType<string>(message);
    // @ts-expect-error Interruption reasons must not infer any.
    expectType<number>(reason);
  })
  .off("drop", (count) => expectType<number>(count))
  .prependListener("devicechange", (name) => expectType<string>(name))
  .prependOnceListener("degraded", (reason) => expectType<TrackInterruptionReason>(reason))
  .addListener("resumed", () => {})
  .removeListener("format", (format) => expectType<AudioFormat>(format))
  .on("error", (error) => expectType<Error>(error))
  .once("end", () => {});

// Every listener method must reject unknown events and wrong payload types.
for (const method of [
  "on",
  "once",
  "off",
  "addListener",
  "removeListener",
  "prependListener",
  "prependOnceListener",
] as const) {
  // @ts-expect-error No permissive string overload for custom events.
  track[method]("interupted", () => {});
  // @ts-expect-error All listener methods check interruption payloads.
  track[method]("interrupted", (_reason: number) => {});
  // @ts-expect-error Mixed streams also reject unknown events.
  mixed[method]("levle", () => {});
  // @ts-expect-error Mixed level callbacks receive numbers.
  mixed[method]("level", (_level: string) => {});
  // @ts-expect-error All meeting listener aliases check names.
  meetings[method]("strated", () => {});
  // @ts-expect-error All meeting listener aliases check payloads.
  meetings[method]("started", (_value: number) => {});
}

track
  .pause()
  .resume()
  .on("data", () => {})
  // @ts-expect-error Stream chaining must retain its event types.
  .once("interupted", () => {});
mixed
  .on("data", (chunk) => {
    expectType<Buffer>(chunk);
    // @ts-expect-error Mixed data is a byte stream, not a raw audio packet.
    expectType<AudioChunk>(chunk);
  })
  .once("level", (level) => {
    expectType<number>(level);
    // @ts-expect-error Level callbacks must not infer any.
    expectType<string>(level);
  })
  .on("error", (error) => expectType<Error>(error))
  .once("end", () => {});

// Standard Node stream operations and lifecycle events remain available.
expectType<Readable>(track);
expectType<Readable>(mixed);
expectType<Writable>(mixed.pipe(destination));
pipeline(track, destination);
pipeline(mixed, destination);
track.on("newListener", (event, listener) => {
  expectType<string | symbol>(event);
  expectType<(...args: any[]) => void>(listener);
});
async function readPackets() {
  for await (const chunk of track) expectType<AudioChunk>(chunk);
}
readPackets();

// @ts-expect-error Domain types still reject unknown values.
expectType<Platform>("zoon");
// @ts-expect-error Constants are readonly.
Platform.Zoom = "teams";
// @ts-expect-error Constants do not accept unknown members.
expectType<unknown>(Platform.Zoon);
// @ts-expect-error Invalid capture targets cannot be constructed.
expectType<CaptureTarget>({ type: "process" });
// @ts-expect-error Permission inputs remain a closed union.
permissions.status("camera");
