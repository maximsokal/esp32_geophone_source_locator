import { meshEdgeKey, type MeshVertexKey } from './meshKeys';

export type MeshBranchPoint = readonly [number, number, number];

export interface MeshBranchTriangle {
  vertices: readonly [MeshBranchPoint, MeshBranchPoint, MeshBranchPoint];
  keys: readonly [MeshVertexKey, MeshVertexKey, MeshVertexKey];
}

export interface MeshThinBranchResult {
  quarantineMask: Uint8Array;
  branchCount: number;
  quarantinedTriangleCount: number;
}

const MAX_BRANCH_TRIANGLES = 12;
const MAX_BRANCH_COMPONENT_FRACTION = 0.20;
const MIN_REMAINING_TRIANGLES = 6;
const MIN_REMAINING_COMPONENT_FRACTION = 0.65;
const MAX_EFFECTIVE_WIDTH_VOXELS = 0.65;
const MIN_BRANCH_MAJOR_VOXELS = 1.80;
const MIN_BRANCH_MAJOR_ABSOLUTE_METERS = 0.065;
const MAX_BRANCH_AREA_VOXELS_SQUARED = 1.05;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function maximumBranchTriangles(componentSize: number): number {
  return Math.min(
    MAX_BRANCH_TRIANGLES,
    Math.max(1, Math.floor(componentSize * MAX_BRANCH_COMPONENT_FRACTION)),
  );
}

interface BranchMetrics {
  areaSquareMeters: number;
  majorExtentMeters: number;
  effectiveWidthMeters: number;
}

interface DfsFrame {
  node: number;
  nextNeighbour: number;
}

function triangleArea(triangle: MeshBranchTriangle): number {
  const [a, b, c] = triangle.vertices;
  const abx = b[0] - a[0];
  const aby = b[1] - a[1];
  const abz = b[2] - a[2];
  const acx = c[0] - a[0];
  const acy = c[1] - a[1];
  const acz = c[2] - a[2];
  const nx = aby * acz - abz * acy;
  const ny = abz * acx - abx * acz;
  const nz = abx * acy - aby * acx;
  return 0.5 * Math.hypot(nx, ny, nz);
}

function branchMetrics(
  members: readonly number[],
  triangles: readonly MeshBranchTriangle[],
): BranchMetrics {
  let areaSquareMeters = 0;
  let minX = Number.POSITIVE_INFINITY;
  let minY = Number.POSITIVE_INFINITY;
  let minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY;
  let maxY = Number.NEGATIVE_INFINITY;
  let maxZ = Number.NEGATIVE_INFINITY;
  for (const index of members) {
    const triangle = triangles[index];
    areaSquareMeters += triangleArea(triangle);
    for (const point of triangle.vertices) {
      minX = Math.min(minX, point[0]);
      minY = Math.min(minY, point[1]);
      minZ = Math.min(minZ, point[2]);
      maxX = Math.max(maxX, point[0]);
      maxY = Math.max(maxY, point[1]);
      maxZ = Math.max(maxZ, point[2]);
    }
  }
  const majorExtentMeters = Math.hypot(maxX - minX, maxY - minY, maxZ - minZ);
  return {
    areaSquareMeters,
    majorExtentMeters,
    effectiveWidthMeters: areaSquareMeters / Math.max(majorExtentMeters, 1e-6),
  };
}

function isSubscaleBranch(
  members: readonly number[],
  triangles: readonly MeshBranchTriangle[],
  voxelSizeMeters: number,
): boolean {
  const safeVoxel = clamp(Math.abs(voxelSizeMeters), 0.01, 0.12);
  const metrics = branchMetrics(members, triangles);
  const minimumMajor = Math.max(MIN_BRANCH_MAJOR_ABSOLUTE_METERS, safeVoxel * MIN_BRANCH_MAJOR_VOXELS);
  const maximumWidth = safeVoxel * MAX_EFFECTIVE_WIDTH_VOXELS;
  const maximumArea = safeVoxel * safeVoxel * MAX_BRANCH_AREA_VOXELS_SQUARED;
  return metrics.majorExtentMeters >= minimumMajor &&
    metrics.effectiveWidthMeters < maximumWidth &&
    metrics.areaSquareMeters < maximumArea;
}

/**
 * Remove only long, sub-voxel-width leaf branches from an otherwise retained mesh.
 *
 * A per-triangle minimum area is deliberately unsafe for marching tetrahedra:
 * perfectly valid zero-crossings naturally create small boundary triangles. This
 * pass instead finds true bridges in the triangle shared-edge graph. Only the
 * smaller side of a bridge can be considered, and only when it is a small fraction
 * of its retained parent component, extends far enough to be a real appendage, yet
 * stays thinner than one reconstruction voxel and below a bounded area budget.
 *
 * The result is mesh-only quarantine. It never removes physical surfels or changes
 * OCCUPIED/FREE evidence. A normal boundary cap, broad structural fold, or large
 * secondary surface therefore survives even when its graph contains a bridge.
 */
export function quarantineSubscaleMeshBranches(
  triangles: readonly MeshBranchTriangle[],
  componentByTriangle: Int32Array,
  componentCount: number,
  repairableComponentMask: Uint8Array,
  voxelSizeMeters: number,
): MeshThinBranchResult {
  const quarantineMask = new Uint8Array(triangles.length);
  if (triangles.length === 0 || componentCount <= 0) {
    return { quarantineMask, branchCount: 0, quarantinedTriangleCount: 0 };
  }

  const membersByComponent: number[][] = Array.from({ length: componentCount }, () => []);
  for (let index = 0; index < triangles.length; index += 1) {
    const component = index < componentByTriangle.length ? componentByTriangle[index] : -1;
    if (
      component < 0 || component >= componentCount ||
      component >= repairableComponentMask.length || repairableComponentMask[component] === 0
    ) continue;
    membersByComponent[component].push(index);
  }

  // Rebuild only the cheap shared-edge graph for retained components. This is O(T)
  // and runs post-scan; no spatial intersection or normal-ray diagnostic is repeated.
  const edgeUses = new Map<MeshVertexKey, number[]>();
  for (let component = 0; component < componentCount; component += 1) {
    if (component >= repairableComponentMask.length || repairableComponentMask[component] === 0) continue;
    for (const index of membersByComponent[component]) {
      const keys = triangles[index].keys;
      for (const [a, b] of [[keys[0], keys[1]], [keys[1], keys[2]], [keys[2], keys[0]]] as const) {
        const id = meshEdgeKey(a, b);
        const uses = edgeUses.get(id);
        if (uses === undefined) edgeUses.set(id, [index]);
        else uses.push(index);
      }
    }
  }

  const adjacency: number[][] = Array.from({ length: triangles.length }, () => []);
  for (const uses of edgeUses.values()) {
    if (uses.length !== 2) continue;
    const first = uses[0];
    const second = uses[1];
    if (componentByTriangle[first] !== componentByTriangle[second]) continue;
    adjacency[first].push(second);
    adjacency[second].push(first);
  }
  for (const neighbours of adjacency) neighbours.sort((a, b) => a - b);

  // One set of O(T) buffers serves every disjoint component. Recursive Tarjan is
  // intentionally avoided: a long mobile mesh strip must not overflow the JS call
  // stack merely because its topology is deep.
  const discovery = new Int32Array(triangles.length);
  discovery.fill(-1);
  const low = new Int32Array(triangles.length);
  const parent = new Int32Array(triangles.length);
  parent.fill(-1);
  const subtreeSize = new Int32Array(triangles.length);
  const orderStart = new Int32Array(triangles.length);
  let clock = 0;

  let branchCount = 0;
  let quarantinedTriangleCount = 0;
  for (let component = 0; component < componentCount; component += 1) {
    if (component >= repairableComponentMask.length || repairableComponentMask[component] === 0) continue;
    const members = membersByComponent[component];
    if (members.length < MIN_REMAINING_TRIANGLES + 2) continue;
    const candidateLimit = maximumBranchTriangles(members.length);

    const order: number[] = [];
    const bridgeChildren: number[] = [];
    for (const seed of members) {
      if (discovery[seed] >= 0) continue;
      discovery[seed] = clock;
      low[seed] = clock;
      clock += 1;
      parent[seed] = -1;
      subtreeSize[seed] = 1;
      orderStart[seed] = order.length;
      order.push(seed);
      const stack: DfsFrame[] = [{ node: seed, nextNeighbour: 0 }];

      while (stack.length > 0) {
        const frame = stack[stack.length - 1];
        const neighbours = adjacency[frame.node];
        if (frame.nextNeighbour < neighbours.length) {
          const neighbour = neighbours[frame.nextNeighbour];
          frame.nextNeighbour += 1;
          if (componentByTriangle[neighbour] !== component) continue;
          if (discovery[neighbour] < 0) {
            parent[neighbour] = frame.node;
            discovery[neighbour] = clock;
            low[neighbour] = clock;
            clock += 1;
            subtreeSize[neighbour] = 1;
            orderStart[neighbour] = order.length;
            order.push(neighbour);
            stack.push({ node: neighbour, nextNeighbour: 0 });
            continue;
          }
          if (neighbour !== parent[frame.node]) {
            low[frame.node] = Math.min(low[frame.node], discovery[neighbour]);
          }
          continue;
        }

        stack.pop();
        const parentNode = parent[frame.node];
        if (parentNode < 0) continue;
        subtreeSize[parentNode] += subtreeSize[frame.node];
        low[parentNode] = Math.min(low[parentNode], low[frame.node]);
        if (low[frame.node] > discovery[parentNode]) bridgeChildren.push(frame.node);
      }
    }

    const candidates: number[][] = [];
    for (const child of bridgeChildren) {
      const childSize = subtreeSize[child];
      const complementSize = members.length - childSize;
      const useSubtree = childSize <= complementSize;
      const candidateSize = useSubtree ? childSize : complementSize;
      // Important mobile bound: do not materialize large bridge sides merely to
      // discover afterwards that they exceed the local-whisker policy.
      if (candidateSize <= 0 || candidateSize > candidateLimit) continue;

      let candidate: number[];
      const start = orderStart[child];
      const end = start + childSize;
      if (useSubtree) {
        candidate = order.slice(start, end);
      } else {
        candidate = members.filter((index) => {
          const position = orderStart[index];
          return position < start || position >= end;
        });
      }
      if (!isSubscaleBranch(candidate, triangles, voxelSizeMeters)) continue;
      candidates.push(candidate.sort((a, b) => a - b));
    }

    // Prefer the largest qualifying branch first. Nested bridge candidates then
    // collapse into one quarantine rather than double-counting the same whisker.
    candidates.sort((a, b) => b.length - a.length || a[0] - b[0]);
    let removedFromComponent = 0;
    const minimumRemaining = Math.max(
      MIN_REMAINING_TRIANGLES,
      Math.ceil(members.length * MIN_REMAINING_COMPONENT_FRACTION),
    );
    for (const candidate of candidates) {
      let newTriangles = 0;
      for (const index of candidate) if (quarantineMask[index] === 0) newTriangles += 1;
      if (newTriangles === 0 || members.length - removedFromComponent - newTriangles < minimumRemaining) continue;
      branchCount += 1;
      for (const index of candidate) {
        if (quarantineMask[index] !== 0) continue;
        quarantineMask[index] = 1;
        removedFromComponent += 1;
        quarantinedTriangleCount += 1;
      }
    }
  }

  return { quarantineMask, branchCount, quarantinedTriangleCount };
}
