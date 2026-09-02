/// <reference lib="webworker" />

import { CertifiedStructuralCornerTracker } from './certifiedStructuralGeometry';
import { stabilizeMeasuredSurfelsWithNativePlanes } from './nativePlaneSurfelGuidance';
import {
  computeSurfaceStructuralReadiness,
  type SurfaceComponentReadinessSummary,
} from './scanCapabilityReadiness';
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
import { analyzeSurfaceComponents } from './surfaceComponents';
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
const threePlaneCornerTracker = new CertifiedStructuralCornerTracker();
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
    threePlaneCornerTracker.reset();
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

  // Connected-component/PCA readiness belongs in the CPU worker too. Returning
  // only compact component metrics avoids the old main-thread O(N) analysis and
  // avoids cloning the large eligible-point masks back across postMessage.
  const componentAnalysis = analyzeSurfaceComponents({
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    voxelSizeMeters: message.voxelSizeMeters,
    minimumConfidence: 0.58,
  });
  const surfaceReadiness: SurfaceComponentReadinessSummary = {
    components: componentAnalysis.components,
    bestMeshComponent: componentAnalysis.bestMeshComponent,
  };
  const structuralSurface = computeSurfaceStructuralReadiness(surfaceReadiness);

  // A small/narrow/sparsely supported measured component is useful scan evidence,
  // but not yet large enough to justify the expensive plane-intersection and
  // uncertainty-aware corner branches. Keep tracker history intact and return a
  // readiness-only result; a later keyframe can re-probe after the scan grows.
  if (!structuralSurface.ready) {
    const result = {
      corners: [],
      edges: [],
      analyzedPoints: componentAnalysis.acceptedPointCount,
      planarPatches: 0,
      computeMs: performance.now() - started,
      keyframeCount: message.keyframeCount,
      rejectedByFreeSpace: 0,
      candidateComparisons: 0,
      surfaceReadiness,
    };
    const diagnostics = {
      ...summarizeStructuralCandidates([], [], []),
      ...lifecycleTelemetry.update([], []),
      nativePlanePriors: message.planePriors?.length ?? 0,
      nativePlaneStabilizedSurfels,
      structuralSurfaceReadiness: Number(structuralSurface.progress.toFixed(3)),
      structuralSurfaceLimit: structuralSurface.limitingMetric,
      structuralGeometryDeferred: true,
    };
    self.postMessage({
      type: 'result',
      requestId: message.requestId,
      epoch: message.epoch,
      result,
      diagnostics,
    });
    return;
  }

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

  // Production corners are supplied by the uncertainty-aware certified trackers
  // below. Skip the legacy raw triple-patch corner loop while retaining the
  // finite-support edge tracker.
  const tracked = tracker.update(input, 0, false);
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
  // alternate. Each solver owns candidate-specific multi-view state: a single
  // robust geometry solve is never mislabeled as three physical observations.
  analysisSequence += 1;
  if (analysisSequence === 1) {
    lastThreePlaneCorners = threePlaneCornerTracker.update(certifiedSnapshot, message.viewpoint ?? null);
    lastEndpointCorners = endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);
  } else if ((analysisSequence & 1) === 1) {
    lastThreePlaneCorners = threePlaneCornerTracker.update(certifiedSnapshot, message.viewpoint ?? null);
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
    surfaceReadiness,
  };
  const diagnostics = {
    ...summarizeStructuralCandidates(tracked.edges, operationalEdges, result.corners),
    ...lifecycleTelemetry.update(tracked.edges, result.corners),
    nativePlanePriors: message.planePriors?.length ?? 0,
    nativePlaneStabilizedSurfels,
    structuralSurfaceReadiness: Number(structuralSurface.progress.toFixed(3)),
    structuralSurfaceLimit: structuralSurface.limitingMetric,
    structuralGeometryDeferred: false,
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
