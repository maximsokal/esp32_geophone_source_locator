import type { PointCloudBounds } from './depthPointCloudV2';
import type { SurfaceComponent, SurfaceComponentAnalysis } from './surfaceComponents';

export interface OperationalSurfaceEnvelope {
  bounds: PointCloudBounds | null;
  retainedComponentIds: readonly number[];
  quarantinedComponentIds: readonly number[];
  retainedPointCount: number;
  quarantinedPointCount: number;
}

const MIN_SECONDARY_COMPONENT_POINTS = 12;
const MIN_SECONDARY_OCCUPIED_CELLS = 4;
const MIN_SECONDARY_MESH_READINESS = 0.55;
const MIN_SECONDARY_OCCUPANCY_RATIO = 0.16;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function secondaryComponentSupported(component: SurfaceComponent, voxelSizeMeters: number): boolean {
  const voxel = clamp(Number.isFinite(voxelSizeMeters) ? Math.abs(voxelSizeMeters) : 0.04, 0.025, 0.10);
  const minimumMajor = Math.max(0.085, voxel * 2.1);
  const minimumMinor = Math.max(0.035, voxel * 0.80);
  return (
    component.pointCount >= MIN_SECONDARY_COMPONENT_POINTS &&
    component.occupiedCellCount >= MIN_SECONDARY_OCCUPIED_CELLS &&
    component.majorExtentMeters >= minimumMajor &&
    component.minorExtentMeters >= minimumMinor &&
    component.occupancyRatio >= MIN_SECONDARY_OCCUPANCY_RATIO &&
    component.meshReadiness >= MIN_SECONDARY_MESH_READINESS
  );
}

/**
 * Build the UNKNOWN source-search envelope from connected physical components.
 *
 * The largest measured component is always retained, even early in a scan. A
 * disconnected secondary island may expand the search volume only after it has
 * its own repeated spatial support: points, occupied cells, two-dimensional
 * extent, occupancy density and partial mesh-scale readiness must all agree.
 *
 * This does NOT delete the secondary island from the physical point cloud or
 * occupancy evidence. Weak islands are only quarantined from expanding the
 * combinatorial UNKNOWN candidate volume until the operator scans them better.
 */
export function computeOperationalSurfaceEnvelope(
  positions: ArrayLike<number>,
  analysis: SurfaceComponentAnalysis,
  voxelSizeMeters: number,
  fallbackBounds: PointCloudBounds | null,
): OperationalSurfaceEnvelope {
  const labels = analysis.componentIdByPoint;
  if (
    analysis.components.length === 0 ||
    analysis.largestComponent === null ||
    labels === undefined ||
    labels.length === 0
  ) {
    return {
      bounds: fallbackBounds,
      retainedComponentIds: [],
      quarantinedComponentIds: [],
      retainedPointCount: 0,
      quarantinedPointCount: 0,
    };
  }

  const primaryId = analysis.largestComponent.id;
  const retained = new Set<number>([primaryId]);
  const quarantined = new Set<number>();
  for (const component of analysis.components) {
    if (component.id === primaryId || secondaryComponentSupported(component, voxelSizeMeters)) {
      retained.add(component.id);
    } else {
      quarantined.add(component.id);
    }
  }

  const pointCount = Math.min(labels.length, Math.floor(positions.length / 3));
  let minX = Number.POSITIVE_INFINITY; let minY = Number.POSITIVE_INFINITY; let minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY; let maxY = Number.NEGATIVE_INFINITY; let maxZ = Number.NEGATIVE_INFINITY;
  let retainedPointCount = 0;
  let quarantinedPointCount = 0;
  for (let index = 0; index < pointCount; index += 1) {
    const componentId = labels[index];
    if (componentId < 0) continue;
    if (!retained.has(componentId)) {
      if (quarantined.has(componentId)) quarantinedPointCount += 1;
      continue;
    }
    const offset = index * 3;
    const x = Number(positions[offset]);
    const y = Number(positions[offset + 1]);
    const z = Number(positions[offset + 2]);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    minX = Math.min(minX, x); minY = Math.min(minY, y); minZ = Math.min(minZ, z);
    maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); maxZ = Math.max(maxZ, z);
    retainedPointCount += 1;
  }

  const bounds = retainedPointCount > 0
    ? { min: [minX, minY, minZ] as const, max: [maxX, maxY, maxZ] as const }
    : fallbackBounds;
  return {
    bounds,
    retainedComponentIds: [...retained].sort((a, b) => a - b),
    quarantinedComponentIds: [...quarantined].sort((a, b) => a - b),
    retainedPointCount,
    quarantinedPointCount,
  };
}
