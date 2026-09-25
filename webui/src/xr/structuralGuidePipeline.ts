import { CertifiedStructuralCornerTracker } from './certifiedStructuralGeometry';
import { stabilizeMeasuredSurfelsWithNativePlanes } from './nativePlaneSurfelGuidance';
import {
  applyMaturityAwareEdgeConfidence,
  summarizeStructuralCandidates,
} from './structuralCandidatePolicy';
import { StructuralEdgeEndpointTracker } from './structuralEdgeEndpoints';
import { rejectStructuralCandidatesInVerifiedFreeSpace } from './structuralFreeSpaceGate';
import {
  StructuralGuideTracker,
  type StructuralCornerCandidate,
  type StructuralGuideInput,
  type StructuralGuideViewpoint,
} from './structuralGuide';
import { prepareStructuralGuideReadiness } from './structuralGuideReadiness';
import { StructuralLifecycleTelemetry } from './structuralLifecycleTelemetry';
import type { StructuralPlanePrior } from './webxrPlaneGuidance';

export interface AnalyzeMessage {
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

export interface ResetMessage { type: 'reset'; epoch: number; }
export type WorkerMessage = AnalyzeMessage | ResetMessage;

function pointDistance(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

/**
 * The structural worker's whole analysis as a plain class, so the production
 * path (readiness gate -> edge tracker -> certified corner trackers ->
 * free-space gate) can be exercised end to end in tests. The worker only
 * forwards messages.
 */
/*
 * Readiness can dip for a single keyframe while a scan grows (measured on a
 * harness ridge: S 1 -> 0.44 -> 1 -> 0.56 -> 1 when one face's components
 * momentarily fail to pair). Without hysteresis every dip returned no edges
 * and tracked edges blinked off. Within a session the map only grows, so a
 * real loss below a quarter means reset or heavy eviction: release there.
 */
const STRUCTURAL_ACTIVE_RELEASE_PROGRESS = 0.25;

export class StructuralGuidePipeline {
  private readonly tracker = new StructuralGuideTracker();
  private readonly threePlaneCornerTracker = new CertifiedStructuralCornerTracker();
  private readonly endpointTracker = new StructuralEdgeEndpointTracker();
  private readonly lifecycleTelemetry = new StructuralLifecycleTelemetry();
  private analysisSequence = 0;
  private lastThreePlaneCorners: readonly StructuralCornerCandidate[] = [];
  private lastEndpointCorners: readonly StructuralCornerCandidate[] = [];
  private structuralActive = false;

  reset(): void {
    this.structuralActive = false;
    this.tracker.reset();
    this.threePlaneCornerTracker.reset();
    this.endpointTracker.reset();
    this.lifecycleTelemetry.reset();
    this.analysisSequence = 0;
    this.lastThreePlaneCorners = [];
    this.lastEndpointCorners = [];
  }

  analyze(message: AnalyzeMessage) {
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

    // S and M deliberately use different physical representations. Keep their
    // preparation in one pure/tested boundary so later performance work cannot
    // accidentally make mesh readiness depend on structural family decimation.
    const readinessPreparationStarted = performance.now();
    const prepared = prepareStructuralGuideReadiness(rawSurface);
    const readinessPreparationMs = performance.now() - readinessPreparationStarted;
    const clusteredSurface = prepared.clusteredSurface;
    const surface = prepared.structuralSurface;
    const meshSurface = prepared.meshSurface;
    const surfaceReadiness = prepared.surfaceReadiness;
    const structuralReadiness = prepared.structuralReadiness;

    // Advisory native planes can rotate normals only after physical clustering and
    // filtering. They never change position/confidence, create surfels or occupancy,
    // and cannot affect the independently prepared mesh-readiness branch above.
    const nativePlaneStabilizedSurfels = stabilizeMeasuredSurfelsWithNativePlanes(
      surface.positions,
      surface.normals,
      surface.confidence,
      message.planePriors,
    );

    // A small/narrow/sparsely supported measured component is useful scan evidence,
    // but not yet large enough to justify plane intersections or certified corners.
    // Keep tracker history intact and let a later keyframe re-probe after scan growth.
    this.structuralActive = structuralReadiness.ready ||
      (this.structuralActive && structuralReadiness.progress >= STRUCTURAL_ACTIVE_RELEASE_PROGRESS);
    if (!this.structuralActive) {
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
        ...this.lifecycleTelemetry.update([], []),
        nativePlanePriors: message.planePriors?.length ?? 0,
        nativePlaneStabilizedSurfels,
        rawStructuralPoints: prepared.rawPointCount,
        clusteredStructuralPoints: clusteredSurface.pointCount,
        certifiedStructuralPoints: surface.pointCount,
        certifiedMeshPoints: meshSurface.pointCount,
        readinessPreparationMs: Number(readinessPreparationMs.toFixed(2)),
        structuralSurfaceReadiness: Number(structuralReadiness.progress.toFixed(3)),
        structuralSurfaceLimit: structuralReadiness.limitingMetric,
        structuralGeometryDeferred: true,
        cornerFreeSpaceRejected: 0,
      };
      return { result, diagnostics };
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
    const tracked = this.tracker.update(input, 0, false);
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
    this.analysisSequence += 1;
    if (this.analysisSequence === 1) {
      this.lastThreePlaneCorners = this.threePlaneCornerTracker.update(certifiedSnapshot, message.viewpoint ?? null);
      this.lastEndpointCorners = this.endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);
    } else if ((this.analysisSequence & 1) === 1) {
      this.lastThreePlaneCorners = this.threePlaneCornerTracker.update(certifiedSnapshot, message.viewpoint ?? null);
    } else {
      this.lastEndpointCorners = this.endpointTracker.update(certifiedSnapshot, message.viewpoint ?? null);
    }

    const mergedCorners = [...this.lastThreePlaneCorners];
    for (const endpoint of this.lastEndpointCorners) {
      const duplicate = mergedCorners.find((corner) => pointDistance(corner.position, endpoint.position) <= 0.075);
      if (duplicate === undefined) {
        mergedCorners.push(endpoint);
      } else if (endpoint.confidence > duplicate.confidence) {
        mergedCorners[mergedCorners.indexOf(duplicate)] = endpoint;
      }
    }

    // Final detector-independent safety invariant. A robust three-plane solve or
    // endpoint tracker may use different internal evidence, but neither may emit a
    // point whose final metric position is already ray-verified FREE. Exact-cell
    // semantics deliberately do not expand FREE into neighbouring UNKNOWN voxels.
    const cornerFreeSpaceGate = rejectStructuralCandidatesInVerifiedFreeSpace(
      mergedCorners,
      certifiedSnapshot,
    );
    const corners = cornerFreeSpaceGate.candidates
      .sort((left, right) => right.confidence - left.confidence);

    const result = {
      ...tracked,
      edges: operationalEdges,
      corners: corners.slice(0, 8),
      computeMs: performance.now() - started,
      rejectedByFreeSpace: tracked.rejectedByFreeSpace + cornerFreeSpaceGate.rejectedByFreeSpace,
      surfaceReadiness,
    };
    const diagnostics = {
      ...summarizeStructuralCandidates(tracked.edges, operationalEdges, result.corners),
      ...this.lifecycleTelemetry.update(tracked.edges, result.corners),
      nativePlanePriors: message.planePriors?.length ?? 0,
      nativePlaneStabilizedSurfels,
      rawStructuralPoints: prepared.rawPointCount,
      clusteredStructuralPoints: clusteredSurface.pointCount,
      certifiedStructuralPoints: surface.pointCount,
      certifiedMeshPoints: meshSurface.pointCount,
      readinessPreparationMs: Number(readinessPreparationMs.toFixed(2)),
      structuralSurfaceReadiness: Number(structuralReadiness.progress.toFixed(3)),
      structuralSurfaceLimit: structuralReadiness.limitingMetric,
      structuralGeometryDeferred: false,
      cornerFreeSpaceRejected: cornerFreeSpaceGate.rejectedByFreeSpace,
    };

    return { result, diagnostics };
  }
}
