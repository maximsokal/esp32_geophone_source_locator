/// <reference lib="webworker" />

import { buildDirectionalTsdfMesh } from './directionalTsdf';
import type { SparseTsdfInput } from './sparseTsdf';

interface ReconstructMessage {
  type: 'reconstruct';
  requestId: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
  maxInputPoints?: number;
  maxTriangles?: number;
}

interface ResetMessage { type: 'reset'; epoch: number; }
type WorkerMessage = ReconstructMessage | ResetMessage;

let epoch = 0;

self.addEventListener('message', (event: MessageEvent<WorkerMessage>) => {
  const message = event.data;
  if (message.type === 'reset') {
    epoch = message.epoch;
    return;
  }

  const requestEpoch = epoch;
  const input: SparseTsdfInput = {
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    voxelSizeMeters: message.voxelSizeMeters,
    freeSpacePositions: message.freeSpacePositions,
    freeSpaceVoxelSizeMeters: message.freeSpaceVoxelSizeMeters,
    maxInputPoints: message.maxInputPoints,
    maxTriangles: message.maxTriangles,
  };
  const result = buildDirectionalTsdfMesh(input);
  if (requestEpoch !== epoch) return;
  self.postMessage({ type: 'result', requestId: message.requestId, epoch: requestEpoch, result }, [result.trianglePositions.buffer]);
});

export {};
