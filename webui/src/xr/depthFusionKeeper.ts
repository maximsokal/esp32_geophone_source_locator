import type { FusionCheckpointState } from './depthFusionCheckpoint';

/**
 * The checkpoint keeper: it only holds the latest checkpoint, outside the fusion worker. The fusion worker sends it over a MessagePort as two messages, a
 * small head and then the state, and the keeper reads the head only: the browser deserialises a message when its data is first read (measured in
 * Chromium: about 40 ms for a 60 000-entry map, at the read, not at the delivery), so until a replacement asks for it the checkpoint is held as bytes and
 * the page never builds the cloud. It lives in the page: a second worker for it cost an isolate of its own (28 MB of mean and 36 MB of peak working set
 * on a 150 s replay) with no gain. After a fusion worker dies, the keeper hands the state to the replacement over a new port.
 */
export interface PortLike {
  postMessage(message: unknown): void;
  onmessage: ((event: { data: unknown }) => void) | null;
  close?: () => void;
}

export interface KeeperCheckpoint {
  generation: number;
  throughSeq: number;
  integratedFrames: number;
  state: FusionCheckpointState;
}

/** Announces the checkpoint that is the next message on the same port. */
export interface CheckpointHead {
  generation: number;
  throughSeq: number;
}

export type KeeperRequest =
  | { type: 'listen'; port: PortLike }
  | { type: 'send'; generation: number; port: PortLike }
  | { type: 'drop' };

export type KeeperResponse =
  | { type: 'stored'; generation: number; throughSeq: number }
  | { type: 'missing'; generation: number };

export function createCheckpointKeeper(post: (message: KeeperResponse) => void): { handle: (message: KeeperRequest) => void; close: () => void } {
  /* `message` is the event the checkpoint came in; its data stays unread until a replacement needs it. */
  let latest: (CheckpointHead & { message: { data: unknown } }) | null = null;
  let listening: PortLike | null = null;
  const handle = (message: KeeperRequest): void => {
    if (message.type === 'listen') {
      listening?.close?.();
      listening = message.port;
      let head: CheckpointHead | null = null;
      listening.onmessage = (event) => {
        if (head === null) { head = event.data as CheckpointHead; return; }
        const { generation, throughSeq } = head;
        head = null;
        if (latest !== null && generation === latest.generation && throughSeq < latest.throughSeq) return;
        latest = { generation, throughSeq, message: event };
        post({ type: 'stored', generation, throughSeq });
      };
      return;
    }
    if (message.type === 'drop') { latest = null; return; }
    /* A checkpoint that cannot be deserialised reads as null: it is as good as missing. */
    const checkpoint = latest === null || latest.generation !== message.generation ? null : latest.message.data as KeeperCheckpoint | null;
    if (checkpoint === null) post({ type: 'missing', generation: message.generation });
    message.port.postMessage(checkpoint);
  };
  return { handle, close: () => { listening?.close?.(); listening = null; latest = null; } };
}

/** The keeper of the application, with the face of a worker (the client replaces a keeper that fails or stays silent). */
export function createInPageCheckpointKeeper(): Worker {
  const events = new EventTarget();
  let closed = false;
  const keeper = createCheckpointKeeper((message) => { if (!closed) events.dispatchEvent(new MessageEvent('message', { data: message })); });
  return Object.assign(events, {
    postMessage: (message: KeeperRequest): void => { if (!closed) keeper.handle(message); },
    terminate: (): void => { closed = true; keeper.close(); },
  }) as unknown as Worker;
}
