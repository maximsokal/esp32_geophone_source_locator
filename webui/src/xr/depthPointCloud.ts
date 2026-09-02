import {
  DepthPointCloudAccumulator as SurfaceDepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthFusionResult,
  type DepthSampleView,
  type LiveCoverageSnapshot,
  type PointCloudBounds,
  type ProjectPoint,
  type ScanPointCloudSnapshot as SurfaceScanPointCloudSnapshot,
} from './depthPointCloudV2';
import { prepareCpuDepthSampler } from './cpuDepthSampler';
import {
  registerManualPlacementResolver,
  solveManualPlacementOnStableSurface,
} from './manualPlacementSurface';
import { optionalMonocularPrior } from './monocularPriorRuntime';
import {
  SparseOccupancyGrid,
  type OccupancySnapshot,
} from './occupancyEvidence';
import {
  rgbGuidanceMatchesView,
  surfaceFusionDepthInMeters,
  type RgbDepthGuidanceFrame,
} from './rgbDepthGuidance';
import { analyzeSurfaceComponents } from './surfaceComponents';
import { computeOperationalSurfaceEnvelope } from './surfaceOperationalEnvelope';
import { clusterStructuralSurfaceFamilies } from './structuralSurfaceClustering';
import {
  filterStructuralSurface,
  STRUCTURAL_MIN_CONFIDENCE,
  type StructuralSurfaceEvidenceSnapshot,
} from './structuralSurfacePreprocess';
import { getFreshRgbGuidance } from './xrGuidanceRegistry';
import { recordActiveXrPerformanceMetric } from './xrPerformanceProfiler';

export type {
  ProjectPoint,
  PointCloudBounds,
  LiveCoverageSnapshot,
  DepthSampleView,
  CpuDepthInformationLike,
  DepthFusionFrameContext,
  DepthFusionResult,
} from './depthPointCloudV2';

export interface ScanPointCloudSnapshot extends SurfaceScanPointCloudSnapshot, OccupancySnapshot {
  /** Component-aware bounds used only to enumerate UNKNOWN source candidates. */
  occupancyEnvelopeBounds: PointCloudBounds | null;
  occupancyEnvelopeComponentCount: number;
  occupancyQuarantinedSurfaceComponentCount: number;
  occupancyQuarantinedSurfacePointCount: number;
  tsdfPositions: Float32Array;
  tsdfNormals: Float32Array;
  tsdfConfidence: Float32Array;
  tsdfResidualMeters: Float32Array;
  tsdfNormalConfidence: Float32Array;
  tsdfCoverageConfidence: Float32Array;
  tsdfCoverageReady: Uint8Array;
  tsdfPointCount: number;
}

export interface StructuralGuideSnapshot extends StructuralSurfaceEvidenceSnapshot {
  freeSpacePositions: Float32Array;
  freeSpaceVoxelSizeMeters: number;
}

/**
 * Narrow live-scan dependency boundary for optional model staging.
 * The accumulator may stage pose-synchronised RGB + metric anchors, but it has
 * deliberately no model-agreement callback and no occupancy handle. Calibrated
 * post-scan rejection lives exclusively in SparseTsdfMeshClient after Finish.
 */
export interface MonocularSurfacePrior {
  readonly isEnabled: boolean;
  reset(): void;
  stagePostScan?(
    guidance: RgbDepthGuidanceFrame,
    depth: CpuDepthInformationLike,
  ): boolean;
}

const TSDF_MIN_CERTIFIED_POINTS = 60;

function guidedSurfaceDepth(
  depth: CpuDepthInformationLike,
  guidance: RgbDepthGuidanceFrame | null,
): CpuDepthInformationLike {
  if (guidance === null) return depth;
  return {
    width: depth.width,
    height: depth.height,
    projectionMatrix: depth.projectionMatrix,
    transform: depth.transform,
    getDepthInMeters(u: number, v: number): number {
      return surfaceFusionDepthInMeters(depth, u, v, guidance);
    },
  };
}

export class DepthPointCloudAccumulator extends SurfaceDepthPointCloudAccumulator {
  private readonly occupancy = new SparseOccupancyGrid();

  constructor(
    voxelSizeMeters?: number,
    memoryBudgetPoints?: number,
    maxDepthMeters?: number,
    private readonly monocularPrior: MonocularSurfacePrior = optionalMonocularPrior,
  ) {
    super(voxelSizeMeters, memoryBudgetPoints, maxDepthMeters);
    registerManualPlacementResolver((candidate: ProjectPoint) =>
      solveManualPlacementOnStableSurface(candidate, this.structuralGuideSnapshot())
    );
  }

  override reset(): void {
    super.reset();
    this.occupancy.reset();
    this.monocularPrior.reset();
  }

  override sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
    context: DepthFusionFrameContext,
  ): DepthFusionResult {
    // XRCPUDepthInformation exposes the same row-major buffer used by
    // getDepthInMeters(). Prepare one zero-copy sampler per frame so the many
    // normal/edge/occupancy probes do not repeatedly cross the browser API
    // boundary. Test doubles and older UAs automatically keep the native path.
    const physicalDepth = prepareCpuDepthSampler(depth).sampler;
    const nowMs = typeof performance !== 'undefined' ? performance.now() : Date.now();
    const freshGuidance = getFreshRgbGuidance(nowMs);
    const guidance = freshGuidance !== null && rgbGuidanceMatchesView(freshGuidance, fallbackView.transform.matrix)
      ? freshGuidance
      : null;

    // Live scan only stages a cheap RGB+physical-anchor packet. ONNX inference
    // is intentionally forbidden from the XR hot loop after real-device tests
    // showed multi-second mobile stalls. The model runs after Finish & save.
    if (context.evidenceKeyframe && guidance !== null && this.monocularPrior.isEnabled) {
      this.monocularPrior.stagePostScan?.(guidance, physicalDepth);
    }

    const result = super.sampleDepthFrame(
      guidedSurfaceDepth(physicalDepth, guidance),
      fallbackView,
      context,
    );
    // Safety invariant: RGB can veto SURFACE evidence, but occupancy/FREE always
    // sees untouched physical XR depth. ONNX is not executed in this path and
    // the injected model interface has no occupancy handle.
    this.occupancy.integrateDepthFrame(physicalDepth, fallbackView, { ...context, maxDepthMeters: this.maxDepth });
    return result;
  }

  override liveCoverageSnapshot(): LiveCoverageSnapshot {
    const started = performance.now();
    try { return super.liveCoverageSnapshot(); }
    finally { recordActiveXrPerformanceMetric('coverageSnapshot', performance.now() - started); }
  }

  /**
   * Cheap live worker handoff. It copies only promoted physical surfels plus the
   * conservative FREE-space snapshot. Structural family clustering and local
   * certification intentionally happen after transfer in the Dedicated Worker.
   */
  structuralWorkerSnapshot(): StructuralGuideSnapshot {
    const surface = super.structuralGuideSnapshot();
    const occupancy = this.occupancy.snapshot(null);
    return {
      ...surface,
      freeSpacePositions: occupancy.freeAirPositions,
      freeSpaceVoxelSizeMeters: occupancy.occupancyVoxelSizeMeters,
    };
  }

  /**
   * Fully certified structural snapshot retained for manual placement and the
   * independent fresh certificate applied to corner suggestions before UI use.
   */
  override structuralGuideSnapshot(): StructuralGuideSnapshot {
    const clustered = clusterStructuralSurfaceFamilies(super.structuralGuideSnapshot());
    const surface = filterStructuralSurface(clustered);
    const occupancy = this.occupancy.snapshot(null);
    return {
      ...surface,
      freeSpacePositions: occupancy.freeAirPositions,
      freeSpaceVoxelSizeMeters: occupancy.occupancyVoxelSizeMeters,
    };
  }

  override snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
    const surface = super.snapshot(depthFrameCount, sampledDepthFrames);
    // UNKNOWN candidate enumeration is the only consumer of this stricter
    // component-aware envelope. Physical surface points, full/robust display
    // bounds and ray-derived occupancy evidence remain untouched.
    const envelopeAnalysis = analyzeSurfaceComponents({
      positions: surface.positions,
      normals: surface.normals,
      confidence: surface.confidence,
      voxelSizeMeters: surface.voxelSizeMeters,
      minimumConfidence: STRUCTURAL_MIN_CONFIDENCE,
    });
    const envelope = computeOperationalSurfaceEnvelope(
      surface.positions,
      envelopeAnalysis,
      surface.voxelSizeMeters,
      surface.bounds,
    );
    const occupancy = this.occupancy.snapshot(envelope.bounds);
    // Keep dense physically certified surfels for Directional TSDF. Optional
    // calibrated AI filtering happens later in SparseTsdfMeshClient, after the
    // XR session has finished, so taking a point-cloud snapshot is model-free.
    const certified = filterStructuralSurface(super.structuralGuideSnapshot());
    const hasTsdfCertificate =
      certified.pointCount >= TSDF_MIN_CERTIFIED_POINTS &&
      certified.residualMeters !== undefined && certified.normalConfidence !== undefined &&
      certified.coverageConfidence !== undefined && certified.coverageReady !== undefined;
    return {
      ...surface,
      ...occupancy,
      occupancyEnvelopeBounds: envelope.bounds,
      occupancyEnvelopeComponentCount: envelope.retainedComponentIds.length,
      occupancyQuarantinedSurfaceComponentCount: envelope.quarantinedComponentIds.length,
      occupancyQuarantinedSurfacePointCount: envelope.quarantinedPointCount,
      tsdfPositions: hasTsdfCertificate ? certified.positions : new Float32Array(0),
      tsdfNormals: hasTsdfCertificate ? certified.normals : new Float32Array(0),
      tsdfConfidence: hasTsdfCertificate ? certified.confidence : new Float32Array(0),
      tsdfResidualMeters: hasTsdfCertificate ? certified.residualMeters! : new Float32Array(0),
      tsdfNormalConfidence: hasTsdfCertificate ? certified.normalConfidence! : new Float32Array(0),
      tsdfCoverageConfidence: hasTsdfCertificate ? certified.coverageConfidence! : new Float32Array(0),
      tsdfCoverageReady: hasTsdfCertificate ? certified.coverageReady! : new Uint8Array(0),
      tsdfPointCount: hasTsdfCertificate ? certified.pointCount : 0,
    };
  }
}
