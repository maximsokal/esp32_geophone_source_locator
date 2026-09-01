/// <reference lib="webworker" />

import { detectCertifiedStructuralCorners } from './certifiedStructuralGeometry';
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

  // Keep the mature edge tracker/diagnostics, but do not expose its old
  // three-small-patch corner hypotheses directly. Auto corners now come from an
  // independent RANSAC/TLS/IRLS local solve on the fused surface map.
  const tracked = tracker.update(input, 0);
  const certifiedCorners = detectCertifiedStructuralCorners({
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    pointCount: Math.floor(message.positions.length / 3),
    voxelSizeMeters: message.voxelSizeMeters,
  });
  const result = {
    ...tracked,
    corners: certifiedCorners,
    computeMs: performance.now() - started,
  };

  self.postMessage({ type: 'result', requestId: message.requestId, epoch: message.epoch, result });
});

export {};
