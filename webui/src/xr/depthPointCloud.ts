import {
  DepthPointCloudAccumulator as SurfaceDepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthFusionResult,
  type DepthSampleView,
  type LiveCoverageSnapshot,
  type ProjectPoint,
  type ScanPointCloudSnapshot as SurfaceScanPointCloudSnapshot,
  type StructuralGuidePointCloudSnapshot,
} from './depthPointCloudV2';
import {
  registerManualPlacementResolver,
  solveManualPlacementOnStableSurface,
} from './manualPlacementSurface';
import {
  SparseOccupancyGrid,
  type OccupancySnapshot,
} from './occupancyEvidence';
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

export interface ScanPointCloudSnapshot
  extends SurfaceScanPointCloudSnapshot, OccupancySnapshot {}

export interface StructuralGuideSnapshot extends StructuralGuidePointCloudSnapshot {
  /** Ray-verified free-space voxel centres used only to reject contradictory guide geometry. */
  freeSpacePositions: Float32Array;
  freeSpaceVoxelSizeMeters: number;
}

const STRUCTURAL_MIN_CONFIDENCE = 0.58;
const STRUCTURAL_NORMAL_DOT = Math.cos((28 * Math.PI) / 180);

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function coordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

function cellKey(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

/**
 * Structural inference is deliberately stricter than the display cloud.
 * A promoted point is already multi-view, but isolated boundary/flying-pixel
 * survivors must not be allowed to define a plane. Keep only promoted points
 * that have nearby, similarly oriented promoted support. If a very early scan
 * is too sparse for that test, preserve the original snapshot rather than
 * manufacturing false certainty from a tiny filtered set.
 */
function filterStructuralSurface(
  surface: StructuralGuidePointCloudSnapshot,
): StructuralGuidePointCloudSnapshot {
  const count = Math.min(
    surface.pointCount,
    Math.floor(surface.positions.length / 3),
    Math.floor(surface.normals.length / 3),
    surface.confidence.length,
  );
  if (count < 24) return surface;

  const cellSize = clamp(surface.voxelSizeMeters * 3.8, 0.075, 0.14);
  const radius = cellSize * 1.45;
  const radiusSquared = radius * radius;
  const buckets = new Map<string, number[]>();

  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    const key = cellKey(
      coordinate(surface.positions[offset], cellSize),
      coordinate(surface.positions[offset + 1], cellSize),
      coordinate(surface.positions[offset + 2], cellSize),
    );
    const bucket = buckets.get(key) ?? [];
    bucket.push(index);
    buckets.set(key, bucket);
  }

  const accepted: number[] = [];
  for (let index = 0; index < count; index += 1) {
    if (surface.confidence[index] < STRUCTURAL_MIN_CONFIDENCE) continue;
    const offset = index * 3;
    const x = surface.positions[offset];
    const y = surface.positions[offset + 1];
    const z = surface.positions[offset + 2];
    const nx = surface.normals[offset];
    const ny = surface.normals[offset + 1];
    const nz = surface.normals[offset + 2];
    const gx = coordinate(x, cellSize);
    const gy = coordinate(y, cellSize);
    const gz = coordinate(z, cellSize);
    let compatibleSupport = 0;

    for (let dx = -1; dx <= 1 && compatibleSupport < 3; dx += 1) {
      for (let dy = -1; dy <= 1 && compatibleSupport < 3; dy += 1) {
        for (let dz = -1; dz <= 1 && compatibleSupport < 3; dz += 1) {
          const bucket = buckets.get(cellKey(gx + dx, gy + dy, gz + dz));
          if (bucket === undefined) continue;
          for (const candidate of bucket) {
            if (candidate === index) continue;
            const candidateOffset = candidate * 3;
            const px = surface.positions[candidateOffset] - x;
            const py = surface.positions[candidateOffset + 1] - y;
            const pz = surface.positions[candidateOffset + 2] - z;
            if (px * px + py * py + pz * pz > radiusSquared) continue;
            const normalDot = Math.abs(
              nx * surface.normals[candidateOffset]
              + ny * surface.normals[candidateOffset + 1]
              + nz * surface.normals[candidateOffset + 2]
            );
            if (normalDot >= STRUCTURAL_NORMAL_DOT) compatibleSupport += 1;
            if (compatibleSupport >= 3) break;
          }
        }
      }
    }
    if (compatibleSupport >= 2) accepted.push(index);
  }

  // Do not let an aggressive local-support gate destroy an early but otherwise
  // valid guide snapshot. Once the map is mature this condition is rarely hit.
  if (accepted.length < Math.max(12, Math.floor(count * 0.35))) return surface;

  const positions = new Float32Array(accepted.length * 3);
  const normals = new Float32Array(accepted.length * 3);
  const confidence = new Float32Array(accepted.length);
  accepted.forEach((source, target) => {
    const sourceOffset = source * 3;
    const targetOffset = target * 3;
    positions[targetOffset] = surface.positions[sourceOffset];
    positions[targetOffset + 1] = surface.positions[sourceOffset + 1];
    positions[targetOffset + 2] = surface.positions[sourceOffset + 2];
    normals[targetOffset] = surface.normals[sourceOffset];
    normals[targetOffset + 1] = surface.normals[sourceOffset + 1];
    normals[targetOffset + 2] = surface.normals[sourceOffset + 2];
    confidence[target] = surface.confidence[source];
  });

  return {
    positions,
    normals,
    confidence,
    pointCount: accepted.length,
    keyframeCount: surface.keyframeCount,
    voxelSizeMeters: surface.voxelSizeMeters,
  };
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

  constructor(
    voxelSizeMeters?: number,
    memoryBudgetPoints?: number,
    maxDepthMeters?: number,
  ) {
    super(voxelSizeMeters, memoryBudgetPoints, maxDepthMeters);
    // WebXrRuntime constructs the point cloud before ManualControlPointStore.
    // Installing this resolver makes every runtime add/move pass through the
    // accumulated multi-view surface instead of trusting one raw depth texel.
    registerManualPlacementResolver((candidate: ProjectPoint) =>
      solveManualPlacementOnStableSurface(candidate, super.structuralGuideSnapshot())
    );
  }

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

  override liveCoverageSnapshot(): LiveCoverageSnapshot {
    const started = performance.now();
    try {
      return super.liveCoverageSnapshot();
    } finally {
      recordActiveXrPerformanceMetric('coverageSnapshot', performance.now() - started);
    }
  }

  override structuralGuideSnapshot(): StructuralGuideSnapshot {
    const surface = filterStructuralSurface(super.structuralGuideSnapshot());
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
