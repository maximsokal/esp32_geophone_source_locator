/// <reference lib="webworker" />

import { detectCertifiedStructuralCorners } from './certifiedStructuralGeometry';
import { stabilizeMeasuredSurfelsWithNativePlanes } from './nativePlaneSurfelGuidance';
import {
  applyMaturityAwareEdgeConfidence,
  summarizeStructuralCandidates,
} from './structuralCandidatePolicy';
import { StructuralEdgeEndpointTracker } from './structuralEdgeEndpoints';
import {
  StructuralGuideTracker,
  type StructuralCornerCandidate,
  type StructuralGuideInput,
  type StructuralGuideViewpoint,
} from './structuralGuide';
import { StructuralLifecycleTelemetry } from './structuralLifecycleTelemetry';
import type { StructuralPlanePrior } from './webxrPlaneGuidance';

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
  planePriors?: readonly StructuralPlanePrior[];
}

interface ResetMessage { type: 'reset'; epoch: number; }
type WorkerMessage = AnalyzeMessage | ResetMessage;

const tracker = new StructuralGuideTracker();
const endpointTracker = new StructuralEdgeEndpointTracker();
const lifecycleTelemetry = new StructuralLifecycleTelemetry();
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
    lifecycleTelemetry.reset();
    analysisSequence = 0;
    lastThreePlaneCorners = [];
    lastEndpointCorners = [];
    return;
  }

  if (message.epoch !== currentEpoch) return;

  const started = performance.now();
  // These arrays were transferred into this worker, so advisory normal
  // stabilization is allocation-free on the main thread. Native plane guidance
  // never changes positions/confidence or creates surfels; physical measurements
  // remain the only geometry and occupancy source.
  const nativePlaneStabilizedSurfels = stabilizeMeasuredSurfelsWithNativePlanes(
    message.positions,
    message.normals,
    message.confidence,
    message.planePriors,
  );
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
  // A two-plane edge that repeatedly survives independent camera views should
  // not remain permanently unusable because the historical geometry/temporal
  // blend tops out a few points below the UI's 0.66 gate. Keep raw geometry and
  // tracking unchanged; only calibrate the aggregate operational confidence.
  const operationalEdges = tracked.edges
    .map(applyMaturityAwareEdgeConfidence)
    .sort((left, right) => right.confidence - left.confidence);

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
  // much more slowly than the XR frame rate, so after the initial seed pass they
  // alternate. Every cached candidate is independently re-certified on the main
  // thread before display, so stale candidates cannot bypass validation.
  analysisSequence += 1;
  if (analysisSequence === 1) {
    lastThreePlaneCorners = detectCertifiedStructuralCorners(certifiedSnapshot);
    lastEndpointCorners = endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);
  } else if ((analysisSequence & 1) === 1) {
    lastThreePlaneCorners = detectCertifiedStructuralCorners(certifiedSnapshot);
  } else {
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
    edges: operationalEdges,
    corners: corners.slice(0, 8),
    computeMs: performance.now() - started,
  };
  const diagnostics = {
    ...summarizeStructuralCandidates(tracked.edges, operationalEdges, result.corners),
    ...lifecycleTelemetry.update(tracked.edges, result.corners),
    nativePlanePriors: message.planePriors?.length ?? 0,
    nativePlaneStabilizedSurfels,
  };

  self.postMessage({
    type: 'result',
    requestId: message.requestId,
    epoch: message.epoch,
    result,
    diagnostics,
  });
});

export {};