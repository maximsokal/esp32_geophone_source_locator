/// <reference lib="webworker" />

import { detectCertifiedStructuralCorners } from './certifiedStructuralGeometry';
import { StructuralEdgeEndpointTracker } from './structuralEdgeEndpoints';
import { StructuralGuideTracker, type StructuralGuideInput, type StructuralGuideViewpoint } from './structuralGuide';

interface AnalyzeMessage {
  type: 'analyze';
  requestId: number;
  epoch: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  residualMeters?: Float32Array;
  normalConfidence?: Float32Array;
  coverageConfidence?: Float32Array;
  coverageReady?: Uint8Array;
  keyframeCount: number;
  voxelSizeMeters: number;
  viewpoint?: StructuralGuideViewpoint | null;
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
}

interface ResetMessage { type: 'reset'; epoch: number; }
type WorkerMessage = AnalyzeMessage | ResetMessage;

const tracker = new StructuralGuideTracker();
const endpointTracker = new StructuralEdgeEndpointTracker();
let currentEpoch = 0;

function pointDistance(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

self.addEventListener('message', (event: MessageEvent<WorkerMessage>) => {
  const message = event.data;
  if (message.type === 'reset') {
    currentEpoch = message.epoch;
    tracker.reset();
    endpointTracker.reset();
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
  // three-small-patch corner hypotheses directly. Auto corners now come from
  // either a bounded 3-plane solve or a multi-view 2-plane edge endpoint whose
  // continuation is independently verified as FREE.
  const tracked = tracker.update(input, 0);
  const certifiedSnapshot = {
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    residualMeters: message.residualMeters,
    normalConfidence: message.normalConfidence,
    coverageConfidence: message.coverageConfidence,
    coverageReady: message.coverageReady,
    pointCount: Math.floor(message.positions.length / 3),
    voxelSizeMeters: message.voxelSizeMeters,
    freeSpacePositions: message.freeSpacePositions,
    freeSpaceVoxelSizeMeters: message.freeSpaceVoxelSizeMeters,
  };
  const threePlaneCorners = detectCertifiedStructuralCorners(certifiedSnapshot);
  const endpointCorners = endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);

  const corners = [...threePlaneCorners];
  for (const endpoint of endpointCorners) {
    const duplicate = corners.find((corner) => pointDistance(corner.position, endpoint.position) <= 0.075);
    if (duplicate === undefined) {
      corners.push(endpoint);
    } else if (endpoint.confidence > duplicate.confidence) {
      corners[corners.indexOf(duplicate)] = endpoint;
    }
  }
  corners.sort((left, right) => right.confidence - left.confidence);

  const result = {
    ...tracked,
    corners: corners.slice(0, 8),
    computeMs: performance.now() - started,
  };

  self.postMessage({ type: 'result', requestId: message.requestId, epoch: message.epoch, result });
});

export {};
