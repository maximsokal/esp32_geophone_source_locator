import type { StructuralGuidePointCloudSnapshot } from './depthPointCloudV2';
import {
  computeSurfaceStructuralReadiness,
  summarizeSurfaceComponentReadiness,
  type StructuralSurfaceReadiness,
  type SurfaceComponentReadinessSummary,
} from './scanCapabilityReadiness';
import { analyzeSurfaceComponents } from './surfaceComponents';
import { clusterStructuralSurfaceFamilies } from './structuralSurfaceClustering';
import {
  filterStructuralSurface,
  type StructuralSurfaceEvidenceSnapshot,
} from './structuralSurfacePreprocess';

export interface PreparedStructuralGuideReadiness {
  rawPointCount: number;
  clusteredSurface: StructuralGuidePointCloudSnapshot;
  structuralSurface: StructuralSurfaceEvidenceSnapshot;
  meshSurface: StructuralSurfaceEvidenceSnapshot;
  surfaceReadiness: SurfaceComponentReadinessSummary;
  structuralReadiness: StructuralSurfaceReadiness;
}

/**
 * Prepare the two deliberately different physical geometry sources used by the
 * live capability bar and the structural solver.
 *
 * S (edge/corner readiness): raw promoted physical surfels -> structural normal-
 * family clustering -> local physical certificate.
 *
 * M (mesh readiness): raw promoted physical surfels -> local physical certificate,
 * without structural-family clustering. This mirrors the post-scan TSDF handoff.
 *
 * Keeping this boundary pure/testable prevents a future optimization from
 * accidentally making M depend on structural decimation or letting native XR
 * planes improve either physical certificate.
 */
export function prepareStructuralGuideReadiness(
  rawSurface: StructuralGuidePointCloudSnapshot,
): PreparedStructuralGuideReadiness {
  const rawPointCount = Math.min(
    rawSurface.pointCount,
    Math.floor(rawSurface.positions.length / 3),
    Math.floor(rawSurface.normals.length / 3),
    rawSurface.confidence.length,
  );

  const meshSurface = filterStructuralSurface(rawSurface);
  const clusteredSurface = clusterStructuralSurfaceFamilies(rawSurface);
  const structuralSurface = filterStructuralSurface(clusteredSurface);

  const structuralComponentAnalysis = analyzeSurfaceComponents({
    positions: structuralSurface.positions,
    normals: structuralSurface.normals,
    confidence: structuralSurface.confidence,
    voxelSizeMeters: structuralSurface.voxelSizeMeters,
    minimumConfidence: 0.58,
  });
  const meshComponentAnalysis = analyzeSurfaceComponents({
    positions: meshSurface.positions,
    normals: meshSurface.normals,
    confidence: meshSurface.confidence,
    voxelSizeMeters: meshSurface.voxelSizeMeters,
    minimumConfidence: 0.58,
  });
  const surfaceReadiness = summarizeSurfaceComponentReadiness(
    structuralComponentAnalysis,
    meshComponentAnalysis,
  );

  return {
    rawPointCount,
    clusteredSurface,
    structuralSurface,
    meshSurface,
    surfaceReadiness,
    structuralReadiness: computeSurfaceStructuralReadiness(surfaceReadiness),
  };
}
