import type { DepthFusionFrameContext } from './depthPointCloudV2';
import type { DepthFrameBuffer, DepthFrameGeometry } from './depthFusionFrame';
import type { PortLike } from './depthFusionKeeper';

/**
 * Messages of the depth-fusion worker (Phase 2). The worker is the SOLE owner of the accumulated surface/occupancy state; the main thread keeps only the
 * latest statistics and the snapshots it asked for. Every message carries the generation (scan session) it belongs to; the answer to a message of an
 * older generation is ignored, so a late reply can never touch a newer scan.
 */
export interface FusionGuidancePacket {
  width: number;
  height: number;
  luma: Uint8Array;
  edgeStrength: Uint8Array;
  gradientX: Int8Array;
  gradientY: Int8Array;
  projectionMatrix?: Float32Array;
  worldFromView?: Float32Array;
}

export interface FusionFrameMessage {
  type: 'frame';
  generation: number;
  seq: number;
  /** An evidence keyframe: never dropped by the queue policy (a polish frame is). */
  mandatory: boolean;
  frame: DepthFrameBuffer;
  geometry: DepthFrameGeometry;
  context: DepthFusionFrameContext;
  guidance: FusionGuidancePacket | null;
}

export type FusionRequestKind = 'coverage' | 'liveMesh' | 'structural' | 'structuralGuide' | 'snapshot' | 'stats';

export interface FusionRequestMessage {
  type: 'request';
  generation: number;
  id: number;
  kind: FusionRequestKind;
  /** coverage: pointBudget; liveMesh: maxPoints; snapshot: depthFrameCount and sampledDepthFrames. */
  params?: { budget?: number; maxPoints?: number; depthFrameCount?: number; sampledDepthFrames?: number };
}

export interface FusionResetMessage { type: 'reset'; generation: number }

/** The port over which this worker sends its checkpoints to the keeper (never through the main thread). */
export interface FusionAttachKeeperMessage { type: 'attachKeeper'; generation: number; port: PortLike }
export interface FusionCheckpointMessage { type: 'checkpoint'; generation: number }
/** A replacement worker: the keeper will send the latest checkpoint over `port`; every later message waits until it has been applied. */
export interface FusionRestoreMessage { type: 'restore'; generation: number; port: PortLike }

export type FusionWorkerRequest = FusionFrameMessage | FusionRequestMessage | FusionResetMessage | FusionAttachKeeperMessage | FusionCheckpointMessage | FusionRestoreMessage;

export interface FusionStats {
  pointCount: number;
  promotedCount: number;
  polishFrameCount: number;
  keyframeCount: number;
  /** Frames integrated since the last reset. */
  integratedFrames: number;
}

export interface FusionFrameDone {
  type: 'frameDone';
  generation: number;
  seq: number;
  /** The depth buffer, returned for reuse (null if the worker kept or lost it). */
  buffer: ArrayBuffer | null;
  changedPoints: number;
  workMs: number;
  stats: FusionStats;
}

export interface FusionResponse {
  type: 'response';
  generation: number;
  id: number;
  kind: FusionRequestKind;
  payload: unknown;
  workMs: number;
  stats: FusionStats;
}

export interface FusionWorkerError {
  type: 'error';
  generation: number;
  seq?: number;
  id?: number;
  message: string;
}

export interface FusionRestored {
  type: 'restored';
  generation: number;
  /** The last frame the restored checkpoint contains; -1 when there was none. */
  throughSeq: number;
  stats: FusionStats;
}

export type FusionWorkerResponse = FusionFrameDone | FusionResponse | FusionWorkerError | FusionRestored;

/** Every ArrayBuffer behind the typed arrays that are direct properties of `value` (one level), each once: what a reply transfers instead of copying. */
export function transferablesOf(value: unknown): ArrayBuffer[] {
  const buffers: ArrayBuffer[] = [];
  const seen = new Set<ArrayBuffer>();
  const take = (candidate: unknown): void => {
    if (ArrayBuffer.isView(candidate) && candidate.buffer instanceof ArrayBuffer && !seen.has(candidate.buffer)) { seen.add(candidate.buffer); buffers.push(candidate.buffer); }
  };
  if (value === null || typeof value !== 'object') return buffers;
  take(value);
  for (const entry of Object.values(value as Record<string, unknown>)) take(entry);
  return buffers;
}
