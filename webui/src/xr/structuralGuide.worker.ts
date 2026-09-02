/// <reference lib="webworker" />

import { CertifiedStructuralCornerTracker } from './certifiedStructuralGeometry';
import { stabilizeMeasuredSurfelsWithNativePlanes } from './nativePlaneSurfelGuidance';
import {
  computeSurfaceStructuralReadiness,
  summarizeSurfaceComponentReadiness,
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
import { clusterStructuralSurfaceFamilies } from './structuralSurfaceClustering';
import { filterStructuralSurface } from './structuralSurfacePreprocess';
import { analyzeSurfaceComponents } from './surfaceComponents';
import type { StructuralPlanePrior } from './webxrPlaneGuidance';

interface AnalyzeMessage {
  type: 'analyze';
  requestId: number;
  epoch: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  /** Legacy/preprocessed callers may still supply these; live raw snapshots do not. */
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
  const rawPointCount = Math.min(
    Math.floor(message.positions.length / 3),
    Math.floor(message.normals.length / 3),
    message.confidence.length,
  );
  const rawSurface = {
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    pointCount: rawPointCount,
    keyframeCount: message.keyframeCount,
    voxelSizeMeters: message.voxelSizeMeters,
  };

  // Mesh readiness must follow the same locally certified physical surfels that
  // the post-scan TSDF branch will receive. Do this directly on raw promoted
  // physical surfels: structural-family clustering is useful for plane/edge
  // fitting, but it is not part of the mesh input policy and must not distort Mxx.
  // Native XR planes are applied later and therefore cannot improve mesh readiness.
  const meshSurface = filterStructuralSurface(rawSurface);

  // All CPU-heavy structural preprocessing now happens after transfer. Family
  // clustering is based on physical measured normals before any native-plane
  // advisory stabilization, preserving the invariant that XRPlane geometry cannot
  // create/merge physical surface families. The local certificate then rejects
  // unsupported measured outliers and supplies residual/normal quality arrays.
  const clusteredSurface = clusterStructuralSurfaceFamilies(rawSurface);
  const surface = filterStructuralSurface(clusteredSurface);

  // Advisory native planes can rotate normals only after physical clustering and
  // filtering. They never change position/confidence, create surfels or occupancy.
  const nativePlaneStabilizedSurfels = stabilizeMeasuredSurfelsWithNativePlanes(
    surface.positions,
    surface.normals,
    surface.confidence,
    message.planePriors,
  );

  // Connected-component/PCA readiness belongs in the CPU worker too. Structural
  // readiness is evaluated on structural-family geometry, while mesh readiness is
  // evaluated independently on the actual certified physical TSDF input. Only the
  // two compact component records cross back to the UI.
  const structuralComponentAnalysis = analyzeSurfaceComponents({
    positions: surface.positions,
    normals: surface.normals,
    confidence: surface.confidence,
    voxelSizeMeters: surface.voxelSizeMeters,
    minimumConfidence: 0.58,
  });
  const meshComponentAnalysis = analyzeSurfaceComponents({
    positions: meshSurface.positions,
    normals: meshSurface.normals,
    confidence: meshSurface.confidence,
    voxelSizeMeters: meshSurface.voxelSizeMeters,
    minimumConfidence: 0.58,
  });
  const surfaceReadiness: SurfaceComponentReadinessSummary = summarizeSurfaceComponentReadiness(
    structuralComponentAnalysis,
    meshComponentAnalysis,
  );
  const structuralSurface = computeSurfaceStructuralReadiness(surfaceReadiness);

  // A small/narrow/sparsely supported measured component is useful scan evidence,
  // but not yet large enough to justify plane intersections or certified corners.
  // Keep tracker history intact and let a later keyframe re-probe after scan growth.
  if (!structuralSurface.ready) {
    const result = {
      corners: [],
      edges: [],
      analyzedPoints: surface.pointCount,
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
      rawStructuralPoints: rawPointCount,
      clusteredStructuralPoints: clusteredSurface.pointCount,
      certifiedStructuralPoints: surface.pointCount,
      certifiedMeshPoints: meshSurface.pointCount,
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
    positions: surface.positions,
    normals: surface.normals,
    confidence: surface.confidence,
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
    positions: surface.positions,
    normals: surface.normals,
    confidence: surface.confidence,
    residualMeters: surface.residualMeters,
    normalConfidence: surface.normalConfidence,
    coverageConfidence: surface.coverageConfidence,
    coverageReady: surface.coverageReady,
    pointCount: surface.pointCount,
    voxelSizeMeters: surface.voxelSizeMeters,
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
    rawStructuralPoints: rawPointCount,
    clusteredStructuralPoints: clusteredSurface.pointCount,
    certifiedStructuralPoints: surface.pointCount,
    certifiedMeshPoints: meshSurface.pointCount,
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
