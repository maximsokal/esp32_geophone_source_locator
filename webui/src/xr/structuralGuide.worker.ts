/// <reference lib="webworker" />

import { StructuralGuideTracker, type StructuralGuideInput, type StructuralGuideViewpoint } from './structuralGuide';

interface AnalyzeMessage {
  type: 'analyze';
  requestId: number;
  epoch: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  keyframeCount: number;
  voxelSizeMeters: number;
  viewpoint?: StructuralGuideViewpoint | null;
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
}

interface ResetMessage { type: 'reset'; epoch: number; }
type WorkerMessage = AnalyzeMessage | ResetMessage;

const tracker = new StructuralGuideTracker();
let currentEpoch = 0;

self.addEventListener('message', (event: MessageEvent<WorkerMessage>) => {
  const message = event.data;
  if (message.type === 'reset') {
    currentEpoch = message.epoch;
    tracker.reset();
    return;
  }

  // A reset can be queued while an older analysis is in flight. The client also
  // validates the echoed epoch, but this early gate prevents stale queued work.
  if (message.epoch !== currentEpoch) return;

  const started = performance.now();
  const input: StructuralGuideInput = {
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    keyframeCount: message.keyframeCount,
    voxelSizeMeters: message.voxelSizeMeters,
    viewpoint: message.viewpoint ?? null,
    freeSpacePositions: message.freeSpacePositions,
    freeSpaceVoxelSizeMeters: message.freeSpaceVoxelSizeMeters,
  };
  const result = tracker.update(input, performance.now() - started);
  self.postMessage({ type: 'result', requestId: message.requestId, epoch: message.epoch, result });
});

export {};
