import type { PointCloudBounds } from './depthPointCloudV2';
import { detectWeakSurfaceBridgeEvidence } from './surfaceBridgeEvidence';
import type { SurfaceComponent, SurfaceComponentAnalysis } from './surfaceComponents';

export interface OperationalSurfaceEnvelope {
  bounds: PointCloudBounds | null;
  retainedComponentIds: readonly number[];
  quarantinedComponentIds: readonly number[];
  /** Quarantined islands touched by a bounded cluster of weak measured bridge surfels. */
  rescanBridgeComponentIds: readonly number[];
  bridgeEvidenceTruncated: boolean;
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

function componentSurfaceAreaProxy(component: SurfaceComponent): number {
  return Math.max(0, component.majorExtentMeters) * Math.max(0, component.minorExtentMeters);
}

/**
 * Primary UNKNOWN-envelope support should be the most physically useful measured
 * component, not merely the one containing the most surfels. A dense tiny/noisy
 * patch can otherwise become unconditionally retained and make the actual larger
 * structure look secondary. Mesh readiness is already a conjunction of point
 * support, occupied cells, 2-D metric extent and occupancy density, so it is the
 * strongest existing scale-aware signal. Remaining ties prefer broader spatial
 * support before raw point count. The ordering is deterministic.
 */
function selectPrimaryComponent(analysis: SurfaceComponentAnalysis): SurfaceComponent | null {
  if (analysis.components.length === 0) return null;
  return [...analysis.components].sort((left, right) =>
    Number(right.meshEligible) - Number(left.meshEligible) ||
    right.meshReadiness - left.meshReadiness ||
    right.occupiedCellCount - left.occupiedCellCount ||
    componentSurfaceAreaProxy(right) - componentSurfaceAreaProxy(left) ||
    right.pointCount - left.pointCount ||
    left.id - right.id
  )[0] ?? null;
}

/**
 * Build the UNKNOWN source-search envelope from connected physical components.
 *
 * The strongest physically supported component is always retained, even early in
 * a scan. "Strongest" is scale-aware rather than a raw point-count maximum. A
 * disconnected secondary island may expand the search volume only after it has
 * its own repeated spatial support: points, occupied cells, two-dimensional
 * extent, occupancy density and partial mesh-scale readiness must all agree.
 *
 * Weak/unassigned measured surfels are independently checked for bounded clusters
 * that touch both retained structure and a quarantined island. Such an island is
 * marked RESCAN-BRIDGE metadata only. It remains quarantined from the UNKNOWN
 * envelope until its own physical support passes the ordinary gates.
 *
 * This does NOT delete a secondary island from the physical point cloud or
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
  const primary = selectPrimaryComponent(analysis);
  if (
    analysis.components.length === 0 ||
    primary === null ||
    labels === undefined ||
    labels.length === 0
  ) {
    return {
      bounds: fallbackBounds,
      retainedComponentIds: [],
      quarantinedComponentIds: [],
      rescanBridgeComponentIds: [],
      bridgeEvidenceTruncated: false,
      retainedPointCount: 0,
      quarantinedPointCount: 0,
    };
  }

  const primaryId = primary.id;
  const retained = new Set<number>([primaryId]);
  const quarantined = new Set<number>();
  for (const component of analysis.components) {
    if (component.id === primaryId || secondaryComponentSupported(component, voxelSizeMeters)) {
      retained.add(component.id);
    } else {
      quarantined.add(component.id);
    }
  }

  const bridgeEvidence = detectWeakSurfaceBridgeEvidence({
    positions,
    componentIdByPoint: labels,
    retainedComponentIds: retained,
    quarantinedComponentIds: quarantined,
    voxelSizeMeters,
  });

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
    rescanBridgeComponentIds: bridgeEvidence.rescanComponentIds,
    bridgeEvidenceTruncated: bridgeEvidence.truncated,
    retainedPointCount,
    quarantinedPointCount,
  };
}
