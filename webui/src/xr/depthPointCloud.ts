import {
  DepthPointCloudAccumulator as SurfaceDepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthFusionResult,
  type DepthSampleView,
  type ScanPointCloudSnapshot as SurfaceScanPointCloudSnapshot,
  type StructuralGuidePointCloudSnapshot,
} from './depthPointCloudV2';
import {
  SparseOccupancyGrid,
  type OccupancySnapshot,
} from './occupancyEvidence';

export type {
  ProjectPoint,
  PointCloudBounds,
  LiveCoverageSnapshot,
  DepthSampleView,
  CpuDepthInformationLike,
  DepthFusionFrameContext,
  DepthFusionResult,
} from './depthPointCloudV2';

export interface ScanPointCloudSnapshot
  extends SurfaceScanPointCloudSnapshot, OccupancySnapshot {}

export interface StructuralGuideSnapshot extends StructuralGuidePointCloudSnapshot {
  /** Ray-verified free-space voxel centres used only to reject contradictory guide geometry. */
  freeSpacePositions: Float32Array;
  freeSpaceVoxelSizeMeters: number;
}

/**
 * Surface reconstruction plus a deliberately conservative occupancy layer.
 *
 * The surfel map answers "where did we repeatedly observe a boundary?".
 * The occupancy map answers the more useful rescue-device question:
 * "which volume is definitely free air, which cells are measured surface,
 * and which volume remains unknown and must stay eligible for source search?"
 *
 * Only independent evidence keyframes modify occupancy. Polish frames may
 * refine surfels, but they cannot turn a stationary camera into certainty
 * about free/occupied space.
 */
export class DepthPointCloudAccumulator extends SurfaceDepthPointCloudAccumulator {
  private readonly occupancy = new SparseOccupancyGrid();

  override reset(): void {
    super.reset();
    this.occupancy.reset();
  }

  override sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
    context: DepthFusionFrameContext,
  ): DepthFusionResult {
    const result = super.sampleDepthFrame(depth, fallbackView, context);

    this.occupancy.integrateDepthFrame(depth, fallbackView, {
      ...context,
      maxDepthMeters: this.maxDepth,
    });

    return result;
  }

  override structuralGuideSnapshot(): StructuralGuideSnapshot {
    const surface = super.structuralGuideSnapshot();
    // Passing null deliberately skips UNKNOWN-volume enumeration. The guide only
    // needs already verified FREE cells, so this stays bounded and cheap.
    const occupancy = this.occupancy.snapshot(null);
    return {
      ...surface,
      freeSpacePositions: occupancy.freeAirPositions,
      freeSpaceVoxelSizeMeters: occupancy.occupancyVoxelSizeMeters,
    };
  }

  override snapshot(
    depthFrameCount: number,
    sampledDepthFrames: number,
  ): ScanPointCloudSnapshot {
    const surface = super.snapshot(depthFrameCount, sampledDepthFrames);
    const occupancy = this.occupancy.snapshot(surface.bounds);
    return { ...surface, ...occupancy };
  }
}
