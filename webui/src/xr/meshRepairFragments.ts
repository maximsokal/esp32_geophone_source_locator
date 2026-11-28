import { meshEdgeKey, type MeshVertexKey } from './meshKeys';

export type MeshRepairPoint = readonly [number, number, number];

export interface MeshRepairTriangle {
  vertices: readonly [MeshRepairPoint, MeshRepairPoint, MeshRepairPoint];
  keys: readonly [MeshVertexKey, MeshVertexKey, MeshVertexKey];
}

export interface MeshRepairFragmentResult {
  quarantineMask: Uint8Array;
  detachedFragmentCount: number;
  detachedTriangleCount: number;
  revertedComponentCount: number;
}

const MIN_FRAGMENT_TRIANGLES = 6;
const MIN_FRAGMENT_AREA_SQUARE_METERS = 0.0015;
const MIN_FRAGMENT_WIDTH_METERS = 0.032;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function subtract(a: MeshRepairPoint, b: MeshRepairPoint): MeshRepairPoint {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function cross(a: MeshRepairPoint, b: MeshRepairPoint): MeshRepairPoint {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function triangleArea(triangle: MeshRepairTriangle): number {
  const [a, b, c] = triangle.vertices;
  const normal = cross(subtract(b, a), subtract(c, a));
  return 0.5 * Math.hypot(normal[0], normal[1], normal[2]);
}

function fragmentIsSubstantial(
  members: readonly number[],
  triangles: readonly MeshRepairTriangle[],
  voxelSizeMeters: number,
): boolean {
  if (members.length < MIN_FRAGMENT_TRIANGLES) return false;
  let area = 0;
  let minX = Number.POSITIVE_INFINITY; let minY = Number.POSITIVE_INFINITY; let minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY; let maxY = Number.NEGATIVE_INFINITY; let maxZ = Number.NEGATIVE_INFINITY;
  for (const index of members) {
    const triangle = triangles[index];
    area += triangleArea(triangle);
    for (const point of triangle.vertices) {
      minX = Math.min(minX, point[0]); minY = Math.min(minY, point[1]); minZ = Math.min(minZ, point[2]);
      maxX = Math.max(maxX, point[0]); maxY = Math.max(maxY, point[1]); maxZ = Math.max(maxZ, point[2]);
    }
  }
  const majorExtentMeters = Math.hypot(maxX - minX, maxY - minY, maxZ - minZ);
  const effectiveWidthMeters = area / Math.max(majorExtentMeters, 1e-6);
  const safeVoxel = clamp(Math.abs(voxelSizeMeters), 0.01, 0.12);
  const minimumMajor = clamp(safeVoxel * 2.5, 0.085, 0.18);
  const minimumWidth = Math.max(MIN_FRAGMENT_WIDTH_METERS, safeVoxel * 0.85);
  const minimumArea = Math.max(
    MIN_FRAGMENT_AREA_SQUARE_METERS,
    minimumMajor * minimumWidth * 0.45,
  );
  return area >= minimumArea &&
    majorExtentMeters >= minimumMajor &&
    effectiveWidthMeters >= minimumWidth;
}

/**
 * Re-check topology only after a bounded local mesh-noise quarantine.
 *
 * Removing a few corrupt faces can cut a narrow bridge and expose a tiny detached
 * rendered island. This pass reuses quantized mesh edges, never physical surfels.
 * It preserves every substantial resulting fragment, so two real large surfaces
 * separated by removal remain visible. Only newly detached fragments below the
 * same area/extent style of mesh scale gates are additionally quarantined.
 *
 * If a proposed local repair would leave no substantial fragment at all, the
 * repair is reverted for that original component. A local heuristic is not
 * allowed to silently erase an otherwise retained physical component.
 */
export function quarantineDetachedMeshRepairFragments(
  triangles: readonly MeshRepairTriangle[],
  componentByTriangle: Int32Array,
  componentCount: number,
  initialQuarantineMask: Uint8Array,
  repairableComponentMask: Uint8Array,
  voxelSizeMeters: number,
): MeshRepairFragmentResult {
  const quarantineMask = initialQuarantineMask.slice();
  if (triangles.length === 0 || componentCount <= 0) {
    return {
      quarantineMask,
      detachedFragmentCount: 0,
      detachedTriangleCount: 0,
      revertedComponentCount: 0,
    };
  }

  const affectedComponents = new Uint8Array(componentCount);
  for (let index = 0; index < triangles.length; index += 1) {
    const component = index < componentByTriangle.length ? componentByTriangle[index] : -1;
    if (
      component >= 0 && component < componentCount &&
      component < repairableComponentMask.length && repairableComponentMask[component] !== 0 &&
      index < initialQuarantineMask.length && initialQuarantineMask[index] !== 0
    ) affectedComponents[component] = 1;
  }

  const edgeUses = new Map<MeshVertexKey, number[]>();
  for (let index = 0; index < triangles.length; index += 1) {
    const component = index < componentByTriangle.length ? componentByTriangle[index] : -1;
    if (component < 0 || component >= componentCount || affectedComponents[component] === 0) continue;
    if (index < initialQuarantineMask.length && initialQuarantineMask[index] !== 0) continue;
    const keys = triangles[index].keys;
    for (const [a, b] of [[keys[0], keys[1]], [keys[1], keys[2]], [keys[2], keys[0]]] as const) {
      const id = meshEdgeKey(a, b);
      const uses = edgeUses.get(id);
      if (uses === undefined) edgeUses.set(id, [index]);
      else uses.push(index);
    }
  }

  const visited = new Uint8Array(triangles.length);
  const fragmentsByComponent: number[][][] = Array.from({ length: componentCount }, () => []);
  for (let seed = 0; seed < triangles.length; seed += 1) {
    const component = seed < componentByTriangle.length ? componentByTriangle[seed] : -1;
    if (component < 0 || component >= componentCount || affectedComponents[component] === 0) continue;
    if (seed < initialQuarantineMask.length && initialQuarantineMask[seed] !== 0) continue;
    if (visited[seed] !== 0) continue;
    const members: number[] = [];
    const queue = [seed];
    visited[seed] = 1;
    for (let read = 0; read < queue.length; read += 1) {
      const current = queue[read];
      members.push(current);
      const keys = triangles[current].keys;
      for (const [a, b] of [[keys[0], keys[1]], [keys[1], keys[2]], [keys[2], keys[0]]] as const) {
        const uses = edgeUses.get(meshEdgeKey(a, b));
        if (uses === undefined) continue;
        for (const neighbour of uses) {
          if (visited[neighbour] !== 0) continue;
          if (componentByTriangle[neighbour] !== component) continue;
          visited[neighbour] = 1;
          queue.push(neighbour);
        }
      }
    }
    fragmentsByComponent[component].push(members);
  }

  let detachedFragmentCount = 0;
  let detachedTriangleCount = 0;
  let revertedComponentCount = 0;
  for (let component = 0; component < componentCount; component += 1) {
    if (affectedComponents[component] === 0) continue;
    const fragments = fragmentsByComponent[component];
    const substantial = fragments.map((members) => fragmentIsSubstantial(members, triangles, voxelSizeMeters));
    if (!substantial.includes(true)) {
      // Proposed local removal was too destructive for this retained component.
      // Restore its original hotspot faces rather than deleting the whole result.
      revertedComponentCount += 1;
      for (let index = 0; index < triangles.length; index += 1) {
        if (componentByTriangle[index] === component) quarantineMask[index] = 0;
      }
      continue;
    }

    for (const [fragment, members] of fragments.entries()) {
      if (substantial[fragment]) continue;
      detachedFragmentCount += 1;
      for (const index of members) {
        if (quarantineMask[index] !== 0) continue;
        quarantineMask[index] = 1;
        detachedTriangleCount += 1;
      }
    }
  }

  return {
    quarantineMask,
    detachedFragmentCount,
    detachedTriangleCount,
    revertedComponentCount,
  };
}
