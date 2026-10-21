import type { Readable } from "node:stream";

export interface EmitterEvents {
  error: (error: Error) => void;
  newListener: (event: string | symbol, listener: (...args: any[]) => void) => void;
  removeListener: (event: string | symbol, listener: (...args: any[]) => void) => void;
}

export interface ReadableEvents<Chunk> extends EmitterEvents {
  close: () => void;
  data: (chunk: Chunk) => void;
  end: () => void;
  pause: () => void;
  readable: () => void;
  resume: () => void;
}

type Listener<Events, Event> = Event extends keyof Events ? Events[Event] : never;

/** Replace Readable's permissive listener overloads, including its aliases. */
export interface TypedReadable<
  Events extends { [Event in keyof Events]: (...args: any[]) => void },
> extends Readable {
  on<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
  once<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
  off<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
  addListener<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
  removeListener<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
  prependListener<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
  prependOnceListener<Event extends string | symbol>(
    event: Event | keyof Events,
    listener: Listener<Events, Event>,
  ): this;
}
