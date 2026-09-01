/// <reference lib="webworker" />

import { detectCertifiedStructuralCorners } from './certifiedStructuralGeometry';
import { StructuralEdgeEndpointTracker } from './structuralEdgeEndpoints';
import {
  StructuralGuideTracker,
  type StructuralCornerCandidate,
  type StructuralGuideInput,
  type StructuralGuideViewpoint,
} from './structuralGuide';

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
let analysisSequence = 0;
let lastThreePlaneCorners: readonly StructuralCornerCandidate[] = [];
let lastEndpointCorners: readonly StructuralCornerCandidate[] = [];

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
    analysisSequence = 0;
    lastThreePlaneCorners = [];
    lastEndpointCorners = [];
    return;
  }

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

  // Both uncertainty-aware solvers are deliberately expensive. Geometry changes
  // much more slowly than the XR frame rate, so alternate them instead of doing
  // both in one worker turn. Every candidate is independently re-certified on
  // the main thread before it is displayed, making cached candidates safe.
  analysisSequence += 1;
  if ((analysisSequence & 1) === 1 || lastThreePlaneCorners.length === 0) {
    lastThreePlaneCorners = detectCertifiedStructuralCorners(certifiedSnapshot);
  } else {
    lastEndpointCorners = endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);
  }

  // Endpoint tracking needs several independent observations. Give it a first
  // observation early without forcing all later analyses to pay both costs.
  if (analysisSequence === 1) {
    lastEndpointCorners = endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);
  }

  const corners = [...lastThreePlaneCorners];
  for (const endpoint of lastEndpointCorners) {
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
