import { createDepthFusionHandler } from '../../../src/xr/depthFusionCore';
import { createCheckpointKeeper, type KeeperRequest, type KeeperResponse } from '../../../src/xr/depthFusionKeeper';
import type { DepthFusionFrameContext, ScanPointCloudSnapshot } from '../../../src/xr/depthPointCloudV2';
import type { FusionWorkerRequest, FusionWorkerResponse } from '../../../src/xr/depthFusionProtocol';
import { ARCORE_NOISE, DEFAULT_INTRINSICS, insideCornerScene, orbitPoses, renderDepth } from './syntheticDepthScene';

/* Fake Workers that run the REAL fusion handler and the REAL checkpoint keeper, joined by real MessageChannels (so the structured clone is real). */
type Listener = (event: never) => void;

class FakeEndpoint {
  terminated = false;
  protected readonly listeners = new Map<string, Set<Listener>>();
  addEventListener(type: string, listener: Listener): void { const set = this.listeners.get(type) ?? new Set(); set.add(listener); this.listeners.set(type, set); }
  removeEventListener(type: string, listener: Listener): void { this.listeners.get(type)?.delete(listener); }
  terminate(): void { this.terminated = true; }
  protected emit(type: string, event: unknown): void { if (!this.terminated) for (const listener of [...(this.listeners.get(type) ?? [])]) (listener as (e: unknown) => void)(event); }
}

export class HandlerWorker extends FakeEndpoint {
  readonly handler = createDepthFusionHandler((message: FusionWorkerResponse) => { if (!this.silent) this.emit('message', { data: message }); });
  /** A hung worker: takes messages, never answers. */
  silent = false;
  framesSeen = 0;
  /** Crash while handling the n-th frame (1-based) or the first request of a kind. */
  crashAtFrame: number | null = null;
  crashOnRequest: string | null = null;
  postMessage(message: FusionWorkerRequest): void {
    if (this.terminated) return;
    if (message.type === 'frame') { this.framesSeen += 1; if (this.crashAtFrame === this.framesSeen) { this.crash(`crash at frame ${this.framesSeen}`); return; } }
    if (message.type === 'request' && this.crashOnRequest === message.kind) { this.crash(`crash on ${message.kind}`); return; }
    this.handler.handle(message);
  }
  crash(reason: string): void { this.emit('error', { message: reason }); this.terminated = true; }
}

export class KeeperWorker extends FakeEndpoint {
  stored: Array<{ generation: number; throughSeq: number }> = [];
  /** A keeper that has died (memory pressure): it stores nothing and answers nothing. */
  dead = false;
  /** Kills the keeper: its checkpoint port is closed and it answers nothing any more. */
  kill(): void { this.keeper.handle({ type: 'listen', port: { postMessage: () => undefined, onmessage: null } }); this.dead = true; }
  private readonly keeper = createCheckpointKeeper((message: KeeperResponse) => { if (message.type === 'stored') this.stored.push(message); this.emit('message', { data: message }); });
  postMessage(message: KeeperRequest): void { if (!this.terminated && !this.dead) this.keeper.handle(message); }
}

/** Lets the MessagePort deliveries (macrotasks in Node) happen. */
export const settle = async (turns = 4): Promise<void> => { for (let i = 0; i < turns; i += 1) await new Promise<void>((resolve) => setTimeout(resolve, 0)); };

export function orbitFrames(count: number) {
  const scene = insideCornerScene([0, 0, 0], 0.6);
  const poses = orbitPoses([0, 0, 0.15], 0.9, 35, 0, 330, count);
  return poses.map((pose, index) => ({ ...renderDepth(scene, pose, index + 1, ARCORE_NOISE, DEFAULT_INTRINSICS), pose, index }));
}
export type OrbitFrame = ReturnType<typeof orbitFrames>[number];
export const frameContext = (frame: OrbitFrame): DepthFusionFrameContext => ({ keyframeId: frame.index + 1, evidenceKeyframe: frame.index % 3 !== 2, frameWeight: frame.index % 3 !== 2 ? 1 : 0.2, cameraProjectPosition: frame.pose.eye });

/** Everything a snapshot says about geometry and occupancy, comparable bit for bit. */
export function snapshotDigest(snapshot: ScanPointCloudSnapshot): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(snapshot as unknown as Record<string, unknown>)) {
    if (ArrayBuffer.isView(value)) {
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      let hash = 2166136261;
      for (let i = 0; i < bytes.length; i += 1) hash = Math.imul(hash ^ bytes[i], 16777619) >>> 0;
      out[key] = `${bytes.length}:${hash.toString(16)}`;
    } else out[key] = value;
  }
  return out;
}
