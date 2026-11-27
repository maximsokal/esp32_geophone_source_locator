import type { FusionCheckpointState } from './depthFusionCheckpoint';

/**
 * The checkpoint keeper: a second, tiny worker that only holds the latest checkpoint. The fusion worker sends it the state over a MessagePort, so the
 * main thread never deserialises the cloud; the main thread only learns THAT a checkpoint is stored and up to which frame. After a fusion worker dies,
 * the keeper hands the state to the replacement over a new port.
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

export type KeeperRequest =
  | { type: 'listen'; port: PortLike }
  | { type: 'send'; generation: number; port: PortLike }
  | { type: 'drop' };

export type KeeperResponse =
  | { type: 'stored'; generation: number; throughSeq: number }
  | { type: 'missing'; generation: number };

export function createCheckpointKeeper(post: (message: KeeperResponse) => void): { handle: (message: KeeperRequest) => void } {
  let latest: KeeperCheckpoint | null = null;
  let listening: PortLike | null = null;
  const handle = (message: KeeperRequest): void => {
    if (message.type === 'listen') {
      listening?.close?.();
      listening = message.port;
      listening.onmessage = (event) => {
        const checkpoint = event.data as KeeperCheckpoint;
        if (latest !== null && checkpoint.generation === latest.generation && checkpoint.throughSeq < latest.throughSeq) return;
        latest = checkpoint;
        post({ type: 'stored', generation: checkpoint.generation, throughSeq: checkpoint.throughSeq });
      };
      return;
    }
    if (message.type === 'drop') { latest = null; return; }
    if (latest === null || latest.generation !== message.generation) { post({ type: 'missing', generation: message.generation }); message.port.postMessage(null); return; }
    message.port.postMessage(latest);
  };
  return { handle };
}
