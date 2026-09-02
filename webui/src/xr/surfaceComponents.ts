import { symmetricEigen3 } from './robustPlaneGeometry';

export type SurfaceComponentPoint = readonly [number, number, number];
export type SurfaceComponentNormal = readonly [number, number, number];

export interface SurfaceComponentInput {
  positions: ArrayLike<number>;
  normals: ArrayLike<number>;
  confidence: ArrayLike<number>;
  voxelSizeMeters: number;
  minimumConfidence?: number;
  readyMask?: ArrayLike<number>;
  maxPoints?: number;
}

export interface SurfaceComponent {
  id: number;
  pointCount: number;
  occupiedCellCount: number;
  center: SurfaceComponentPoint;
  majorExtentMeters: number;
  minorExtentMeters: number;
  thicknessMeters: number;
  occupancyRatio: number;
  meshReadiness: number;
  meshEligible: boolean;
}

export interface SurfaceComponentAnalysis {
  pointCount: number;
  acceptedPointCount: number;
  componentCount: number;
  eligibleComponentCount: number;
  eligiblePointCount: number;
  eligiblePointMask: Uint8Array;
  components: readonly SurfaceComponent[];
  largestComponent: SurfaceComponent | null;
  bestMeshComponent: SurfaceComponent | null;
  connectionCellSizeMeters: number;
}

interface OrientationNode {
  id: number;
  gx: number;
  gy: number;
  gz: number;
  spatialKey: string;
  pointIndices: number[];
  normalX: number;
  normalY: number;
  normalZ: number;
  normalWeight: number;
}

const MAX_ORIENTATION_NODES_PER_CELL = 3;
const ORIENTATION_FAMILY_DOT = Math.cos((34 * Math.PI) / 180);
const COMPONENT_NORMAL_DOT = Math.cos((52 * Math.PI) / 180);
const DEFAULT_MINIMUM_CONFIDENCE = 0.58;
const MESH_MIN_POINTS = 24;
const MESH_MIN_OCCUPIED_CELLS = 7;
const MESH_MIN_OCCUPANCY_RATIO = 0.30;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number { return clamp(value, 0, 1); }
function grid(value: number, size: number): number { return Math.floor(value / size); }
function key(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }

function normalize(x: number, y: number, z: number): SurfaceComponentNormal | null {
  const magnitude = Math.hypot(x, y, z);
  return Number.isFinite(magnitude) && magnitude > 1e-8
    ? [x / magnitude, y / magnitude, z / magnitude]
    : null;
}

function canonical(normal: SurfaceComponentNormal): SurfaceComponentNormal {
  const ax = Math.abs(normal[0]);
  const ay = Math.abs(normal[1]);
  const az = Math.abs(normal[2]);
  const axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
  return normal[axis] < 0 ? [-normal[0], -normal[1], -normal[2]] : normal;
}

function absDot(a: SurfaceComponentNormal, b: SurfaceComponentNormal): number {
  return Math.abs(clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1, 1));
}

function nodeNormal(node: OrientationNode): SurfaceComponentNormal | null {
  return normalize(node.normalX, node.normalY, node.normalZ);
}

function meshThresholds(voxelSizeMeters: number): {
  major: number;
  minor: number;
  points: number;
  cells: number;
} {
  const voxel = clamp(Number.isFinite(voxelSizeMeters) ? voxelSizeMeters : 0.04, 0.025, 0.10);
  return {
    // Surface reconstruction below roughly 3.5 source voxels has no useful
    // metric shape at the resolution used by this project.
    major: Math.max(0.14, voxel * 3.5),
    // A mesh needs genuine 2-D support. A long one-dimensional chain of points
    // is an edge candidate, not a surface to triangulate.
    minor: Math.max(0.075, voxel * 1.85),
    points: MESH_MIN_POINTS,
    cells: MESH_MIN_OCCUPIED_CELLS,
  };
}

function readinessRatio(value: number, target: number): number {
  return target <= 0 ? 1 : clamp01(value / target);
}

function componentMetrics(
  id: number,
  pointIndices: readonly number[],
  occupiedCellCount: number,
  input: SurfaceComponentInput,
  cellSizeMeters: number,
): SurfaceComponent {
  let sx = 0; let sy = 0; let sz = 0;
  for (const index of pointIndices) {
    const offset = index * 3;
    sx += Number(input.positions[offset]);
    sy += Number(input.positions[offset + 1]);
    sz += Number(input.positions[offset + 2]);
  }
  const inverse = 1 / Math.max(1, pointIndices.length);
  const center: SurfaceComponentPoint = [sx * inverse, sy * inverse, sz * inverse];
  let xx = 0; let xy = 0; let xz = 0; let yy = 0; let yz = 0; let zz = 0;
  for (const index of pointIndices) {
    const offset = index * 3;
    const dx = Number(input.positions[offset]) - center[0];
    const dy = Number(input.positions[offset + 1]) - center[1];
    const dz = Number(input.positions[offset + 2]) - center[2];
    xx += dx * dx; xy += dx * dy; xz += dx * dz;
    yy += dy * dy; yz += dy * dz; zz += dz * dz;
  }
  const eigen = symmetricEigen3([
    xx * inverse, xy * inverse, xz * inverse,
    yy * inverse, yz * inverse, zz * inverse,
  ]);
  // For a roughly uniformly sampled interval, sqrt(12 * variance) estimates
  // physical span. Cell size is a conservative floor so a one-cell component
  // does not report an exact zero extent.
  const thicknessMeters = Math.max(cellSizeMeters, Math.sqrt(12 * Math.max(0, eigen.values[0])));
  const minorExtentMeters = Math.max(cellSizeMeters, Math.sqrt(12 * Math.max(0, eigen.values[1])));
  const majorExtentMeters = Math.max(cellSizeMeters, Math.sqrt(12 * Math.max(0, eigen.values[2])));
  const expectedSurfaceCells = Math.max(
    1,
    Math.ceil(majorExtentMeters / cellSizeMeters) * Math.ceil(minorExtentMeters / cellSizeMeters),
  );
  const occupancyRatio = clamp01(occupiedCellCount / expectedSurfaceCells);
  const thresholds = meshThresholds(input.voxelSizeMeters);
  const meshReadiness = Math.min(
    readinessRatio(pointIndices.length, thresholds.points),
    readinessRatio(occupiedCellCount, thresholds.cells),
    readinessRatio(majorExtentMeters, thresholds.major),
    readinessRatio(minorExtentMeters, thresholds.minor),
    readinessRatio(occupancyRatio, MESH_MIN_OCCUPANCY_RATIO),
  );
  return {
    id,
    pointCount: pointIndices.length,
    occupiedCellCount,
    center,
    majorExtentMeters,
    minorExtentMeters,
    thicknessMeters,
    occupancyRatio,
    meshReadiness,
    meshEligible: meshReadiness >= 1 - 1e-9,
  };
}

/**
 * Memory-bounded connected-component analysis for unordered measured surfels.
 *
 * We intentionally do not run all-point DBSCAN on the phone. Points first enter
 * a voxel hash, each cell retains at most three normal-orientation families,
 * and BFS traverses only the 3x3x3 neighbouring occupied cells. This preserves
 * disconnected islands and sharp surface boundaries while keeping memory O(N)
 * and neighbour discovery local.
 */
export function analyzeSurfaceComponents(input: SurfaceComponentInput): SurfaceComponentAnalysis {
  const sourceCount = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
    Math.max(0, Math.floor(input.maxPoints ?? Number.MAX_SAFE_INTEGER)),
  );
  const minimumConfidence = clamp01(input.minimumConfidence ?? DEFAULT_MINIMUM_CONFIDENCE);
  const voxel = clamp(Number.isFinite(input.voxelSizeMeters) ? input.voxelSizeMeters : 0.04, 0.025, 0.10);
  const connectionCellSizeMeters = clamp(voxel * 1.50, 0.045, 0.10);
  const nodes: OrientationNode[] = [];
  const nodesByCell = new Map<string, OrientationNode[]>();
  let acceptedPointCount = 0;

  for (let index = 0; index < sourceCount; index += 1) {
    const confidence = Number(input.confidence[index]);
    if (!Number.isFinite(confidence) || confidence < minimumConfidence) continue;
    if (input.readyMask !== undefined && index < input.readyMask.length && Number(input.readyMask[index]) <= 0) continue;
    const offset = index * 3;
    const x = Number(input.positions[offset]);
    const y = Number(input.positions[offset + 1]);
    const z = Number(input.positions[offset + 2]);
    const rawNormal = normalize(
      Number(input.normals[offset]),
      Number(input.normals[offset + 1]),
      Number(input.normals[offset + 2]),
    );
    if (![x, y, z].every(Number.isFinite) || rawNormal === null) continue;
    const normal = canonical(rawNormal);
    const gx = grid(x, connectionCellSizeMeters);
    const gy = grid(y, connectionCellSizeMeters);
    const gz = grid(z, connectionCellSizeMeters);
    const spatialKey = key(gx, gy, gz);
    const cellNodes = nodesByCell.get(spatialKey) ?? [];
    let best: OrientationNode | null = null;
    let bestDot = ORIENTATION_FAMILY_DOT;
    for (const node of cellNodes) {
      const reference = nodeNormal(node);
      if (reference === null) continue;
      const similarity = absDot(reference, normal);
      if (similarity >= bestDot) { best = node; bestDot = similarity; }
    }
    if (best === null) {
      if (cellNodes.length >= MAX_ORIENTATION_NODES_PER_CELL) continue;
      best = {
        id: nodes.length,
        gx, gy, gz, spatialKey,
        pointIndices: [],
        normalX: 0, normalY: 0, normalZ: 0,
        normalWeight: 0,
      };
      nodes.push(best);
      cellNodes.push(best);
      nodesByCell.set(spatialKey, cellNodes);
    }
    const reference = nodeNormal(best);
    const sign = reference !== null && (
      reference[0] * normal[0] + reference[1] * normal[1] + reference[2] * normal[2]
    ) < 0 ? -1 : 1;
    const weight = clamp01(confidence);
    best.normalX += normal[0] * weight * sign;
    best.normalY += normal[1] * weight * sign;
    best.normalZ += normal[2] * weight * sign;
    best.normalWeight += weight;
    best.pointIndices.push(index);
    acceptedPointCount += 1;
  }

  const visited = new Uint8Array(nodes.length);
  const components: SurfaceComponent[] = [];
  const componentPointIndices: number[][] = [];

  for (const seed of nodes) {
    if (visited[seed.id] !== 0) continue;
    visited[seed.id] = 1;
    const queue: OrientationNode[] = [seed];
    const points: number[] = [];
    const occupiedCells = new Set<string>();
    for (let read = 0; read < queue.length; read += 1) {
      const current = queue[read];
      points.push(...current.pointIndices);
      occupiedCells.add(current.spatialKey);
      const currentNormal = nodeNormal(current);
      if (currentNormal === null) continue;
      for (let dx = -1; dx <= 1; dx += 1) {
        for (let dy = -1; dy <= 1; dy += 1) {
          for (let dz = -1; dz <= 1; dz += 1) {
            const neighbours = nodesByCell.get(key(current.gx + dx, current.gy + dy, current.gz + dz));
            if (neighbours === undefined) continue;
            for (const neighbour of neighbours) {
              if (visited[neighbour.id] !== 0) continue;
              const neighbourNormal = nodeNormal(neighbour);
              if (neighbourNormal === null || absDot(currentNormal, neighbourNormal) < COMPONENT_NORMAL_DOT) continue;
              visited[neighbour.id] = 1;
              queue.push(neighbour);
            }
          }
        }
      }
    }
    const component = componentMetrics(
      components.length,
      points,
      occupiedCells.size,
      input,
      connectionCellSizeMeters,
    );
    components.push(component);
    componentPointIndices.push(points);
  }

  const eligiblePointMask = new Uint8Array(sourceCount);
  let eligiblePointCount = 0;
  let eligibleComponentCount = 0;
  for (let componentIndex = 0; componentIndex < components.length; componentIndex += 1) {
    if (!components[componentIndex].meshEligible) continue;
    eligibleComponentCount += 1;
    for (const pointIndex of componentPointIndices[componentIndex]) {
      if (eligiblePointMask[pointIndex] !== 0) continue;
      eligiblePointMask[pointIndex] = 1;
      eligiblePointCount += 1;
    }
  }

  const bySize = [...components].sort((left, right) =>
    right.pointCount - left.pointCount ||
    right.majorExtentMeters - left.majorExtentMeters ||
    left.id - right.id,
  );
  const byMesh = [...components].sort((left, right) =>
    right.meshReadiness - left.meshReadiness ||
    right.pointCount - left.pointCount ||
    left.id - right.id,
  );
  return {
    pointCount: sourceCount,
    acceptedPointCount,
    componentCount: components.length,
    eligibleComponentCount,
    eligiblePointCount,
    eligiblePointMask,
    components,
    largestComponent: bySize[0] ?? null,
    bestMeshComponent: byMesh[0] ?? null,
    connectionCellSizeMeters,
  };
}
