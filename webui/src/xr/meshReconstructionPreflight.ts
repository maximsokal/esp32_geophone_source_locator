import {
  MESH_MIN_POINTS,
  analyzeSurfaceComponents,
  type SurfaceComponentAnalysis,
} from './surfaceComponents';

export interface MeshReconstructionPreflightInput {
  positions: ArrayLike<number>;
  normals: ArrayLike<number>;
  confidence: ArrayLike<number>;
  voxelSizeMeters: number;
}

export interface MeshReconstructionPreflight {
  ready: boolean;
  sourcePointCount: number;
  acceptedPointCount: number;
  componentCount: number;
  eligibleComponentCount: number;
  eligiblePointCount: number;
  meshReadiness: number;
  limitingReason: 'ready' | 'points' | 'extent-or-density';
  analysis: SurfaceComponentAnalysis;
}

const MIN_SURFACE_CONFIDENCE = 0.58;

/**
 * Cheap O(N) physical gate before expensive post-scan AI or TSDF worker work.
 *
 * This deliberately uses the same connected-component implementation and point
 * threshold as reconstruction itself. A global point count is not enough: at
 * least one physically connected 2-D component must satisfy point, cell,
 * metric-extent and occupancy-density requirements before reconstruction is
 * worth starting.
 */
export function assessMeshReconstructionPreflight(
  input: MeshReconstructionPreflightInput,
): MeshReconstructionPreflight {
  const sourcePointCount = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
  );
  const analysis = analyzeSurfaceComponents({
    positions: input.positions,
    normals: input.normals,
    confidence: input.confidence,
    voxelSizeMeters: input.voxelSizeMeters,
    minimumConfidence: MIN_SURFACE_CONFIDENCE,
  });
  const best = analysis.bestMeshComponent;
  const ready = analysis.eligibleComponentCount > 0 && analysis.eligiblePointCount > 0;
  const limitingReason = ready
    ? 'ready'
    : sourcePointCount < MESH_MIN_POINTS || (best?.pointCount ?? 0) < MESH_MIN_POINTS
      ? 'points'
      : 'extent-or-density';
  return {
    ready,
    sourcePointCount,
    acceptedPointCount: analysis.acceptedPointCount,
    componentCount: analysis.componentCount,
    eligibleComponentCount: analysis.eligibleComponentCount,
    eligiblePointCount: analysis.eligiblePointCount,
    meshReadiness: best?.meshReadiness ?? 0,
    limitingReason,
    analysis,
  };
}
