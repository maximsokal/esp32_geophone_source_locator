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
  /** MAX-254: unit plane normal (smallest principal axis) of the component. */
  normal?: SurfaceComponentNormal;
  /** MAX-254: planar area estimate (convex hull in the plane + half-spacing border). */
  estimatedAreaSquareMeters?: number;
}

export interface SurfaceComponentAnalysis {
  pointCount: number;
  acceptedPointCount: number;
  componentCount: number;
  eligibleComponentCount: number;
  eligiblePointCount: number;
  eligiblePointMask: Uint8Array;
  /**
   * Source-point -> connected component label, -1 for rejected/unassigned input.
   * Optional so hand-authored fixtures and older callers remain source-compatible.
   */
  componentIdByPoint?: Int32Array;
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
  connectionPointIndices: number[];
  normalX: number;
  normalY: number;
  normalZ: number;
  normalWeight: number;
  positionX: number;
  positionY: number;
  positionZ: number;
  positionWeight: number;
  minX: number;
  minY: number;
  minZ: number;
  maxX: number;
  maxY: number;
  maxZ: number;
}

interface CellSample {
  index: number;
  x: number;
  y: number;
  z: number;
  confidence: number;
  normal: SurfaceComponentNormal;
}

interface CellBucket {
  gx: number;
  gy: number;
  gz: number;
  spatialKey: string;
  samples: CellSample[];
}

const MAX_ORIENTATION_NODES_PER_CELL = 3;
const MAX_PROVISIONAL_ORIENTATION_NODES_PER_CELL = 12;
const ORIENTATION_FAMILY_DOT = Math.cos((34 * Math.PI) / 180);
const COMPONENT_NORMAL_DOT = Math.cos((52 * Math.PI) / 180);
const DEFAULT_MINIMUM_CONFIDENCE = 0.58;
/**
 * Actual post-scan TSDF handoff already requires 60 locally certified surfels.
 * Keep connected-component readiness on the same point gate so the live Mxx bar
 * cannot reach 100% while reconstruction is still guaranteed to remain disabled.
 */
export const MESH_MIN_POINTS = 60;
const MESH_MIN_OCCUPIED_CELLS = 7;
const MESH_MIN_OCCUPANCY_RATIO = 0.30;
const ROBUST_EXTENT_TRIM_FRACTION = 0.10;
const MIN_POINTS_FOR_ROBUST_EXTENT = 20;
const MAX_NODE_CONNECTION_SAMPLES = 8;
// Attach and propagation are deliberately different. One sparse physical surfel
// may extend a real surface by one cell, but it must not relay BFS indefinitely.
const MIN_NODE_PROPAGATION_POINTS = 3;
const MIN_LOCAL_PROPAGATION_SUPPORT_POINTS = 6;
const MAX_NEIGHBOUR_PROPAGATION_CONTRIBUTION = 2;

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

function signedDot(a: SurfaceComponentNormal, b: SurfaceComponentNormal): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
function absDot(a: SurfaceComponentNormal, b: SurfaceComponentNormal): number {
  return Math.abs(clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1, 1));
}

function nodeNormal(node: OrientationNode): SurfaceComponentNormal | null {
  return normalize(node.normalX, node.normalY, node.normalZ);
}

function nodeCenter(node: OrientationNode): SurfaceComponentPoint | null {
  if (!Number.isFinite(node.positionWeight) || node.positionWeight <= 1e-8) return null;
  return [
    node.positionX / node.positionWeight,
    node.positionY / node.positionWeight,
    node.positionZ / node.positionWeight,
  ];
}

function axisGap(minA: number, maxA: number, minB: number, maxB: number): number {
  if (maxA < minB) return minB - maxA;
  if (maxB < minA) return minA - maxB;
  return 0;
}

function nodeSupportGapMeters(a: OrientationNode, b: OrientationNode): number {
  const dx = axisGap(a.minX, a.maxX, b.minX, b.maxX);
  const dy = axisGap(a.minY, a.maxY, b.minY, b.maxY);
  const dz = axisGap(a.minZ, a.maxZ, b.minZ, b.maxZ);
  return Math.hypot(dx, dy, dz);
}

function comparePointLexicographic(
  input: SurfaceComponentInput,
  left: number,
  right: number,
): number {
  const leftOffset = left * 3;
  const rightOffset = right * 3;
  return Number(input.positions[leftOffset]) - Number(input.positions[rightOffset]) ||
    Number(input.positions[leftOffset + 1]) - Number(input.positions[rightOffset + 1]) ||
    Number(input.positions[leftOffset + 2]) - Number(input.positions[rightOffset + 2]);
}

/**
 * Pick at most eight real surfels once per node. Axis extrema protect contacts at
 * hash-cell faces/edges/corners that an index/quantile-only sampler can miss.
 * Near/far-center and deterministic quantile fill retain some interior coverage.
 * Position tuples are intentionally not allocated in these loops: this path runs
 * once for every orientation node and noisy mobile scans can contain many nodes.
 */
function selectConnectionPointIndices(
  node: OrientationNode,
  input: SurfaceComponentInput,
): number[] {
  const indices = node.pointIndices;
  if (indices.length <= MAX_NODE_CONNECTION_SAMPLES) {
    return [...indices].sort((a, b) => comparePointLexicographic(input, a, b));
  }

  const selected: number[] = [];
  const selectedSet = new Set<number>();
  const add = (index: number): void => {
    if (index < 0 || selected.length >= MAX_NODE_CONNECTION_SAMPLES || selectedSet.has(index)) return;
    selectedSet.add(index);
    selected.push(index);
  };

  let minXIndex = indices[0];
  let maxXIndex = indices[0];
  let minYIndex = indices[0];
  let maxYIndex = indices[0];
  let minZIndex = indices[0];
  let maxZIndex = indices[0];
  const firstOffset = indices[0] * 3;
  let minX = Number(input.positions[firstOffset]);
  let maxX = minX;
  let minY = Number(input.positions[firstOffset + 1]);
  let maxY = minY;
  let minZ = Number(input.positions[firstOffset + 2]);
  let maxZ = minZ;

  for (const index of indices) {
    const offset = index * 3;
    const x = Number(input.positions[offset]);
    const y = Number(input.positions[offset + 1]);
    const z = Number(input.positions[offset + 2]);
    if (x < minX || (x === minX && comparePointLexicographic(input, index, minXIndex) < 0)) {
      minX = x;
      minXIndex = index;
    }
    if (x > maxX || (x === maxX && comparePointLexicographic(input, index, maxXIndex) < 0)) {
      maxX = x;
      maxXIndex = index;
    }
    if (y < minY || (y === minY && comparePointLexicographic(input, index, minYIndex) < 0)) {
      minY = y;
      minYIndex = index;
    }
    if (y > maxY || (y === maxY && comparePointLexicographic(input, index, maxYIndex) < 0)) {
      maxY = y;
      maxYIndex = index;
    }
    if (z < minZ || (z === minZ && comparePointLexicographic(input, index, minZIndex) < 0)) {
      minZ = z;
      minZIndex = index;
    }
    if (z > maxZ || (z === maxZ && comparePointLexicographic(input, index, maxZIndex) < 0)) {
      maxZ = z;
      maxZIndex = index;
    }
  }
  for (const index of [minXIndex, maxXIndex, minYIndex, maxYIndex, minZIndex, maxZIndex]) add(index);

  if (
    selected.length < MAX_NODE_CONNECTION_SAMPLES &&
    Number.isFinite(node.positionWeight) && node.positionWeight > 1e-8
  ) {
    const centerX = node.positionX / node.positionWeight;
    const centerY = node.positionY / node.positionWeight;
    const centerZ = node.positionZ / node.positionWeight;
    let nearest = -1;
    let nearestSquared = Number.POSITIVE_INFINITY;
    let farthest = -1;
    let farthestSquared = Number.NEGATIVE_INFINITY;
    for (const index of indices) {
      const offset = index * 3;
      const dx = Number(input.positions[offset]) - centerX;
      const dy = Number(input.positions[offset + 1]) - centerY;
      const dz = Number(input.positions[offset + 2]) - centerZ;
      const squared = dx * dx + dy * dy + dz * dz;
      if (
        squared < nearestSquared ||
        (squared === nearestSquared && (nearest < 0 || comparePointLexicographic(input, index, nearest) < 0))
      ) { nearest = index; nearestSquared = squared; }
      if (
        squared > farthestSquared ||
        (squared === farthestSquared && (farthest < 0 || comparePointLexicographic(input, index, farthest) < 0))
      ) { farthest = index; farthestSquared = squared; }
    }
    add(nearest);
    add(farthest);
  }

  // Extrema may collapse on lines/planes. Fill remaining slots from the already
  // deterministic point order; this remains bounded and is computed once/node.
  if (selected.length < MAX_NODE_CONNECTION_SAMPLES) {
    const ordered = [...indices].sort((a, b) => comparePointLexicographic(input, a, b));
    const last = ordered.length - 1;
    for (let cursor = 0; cursor < MAX_NODE_CONNECTION_SAMPLES && selected.length < MAX_NODE_CONNECTION_SAMPLES; cursor += 1) {
      add(ordered[Math.round(cursor * last / (MAX_NODE_CONNECTION_SAMPLES - 1))]);
    }
  }
  return selected;
}

function nodesHaveNearbyMeasuredSupport(
  a: OrientationNode,
  b: OrientationNode,
  input: SurfaceComponentInput,
  maximumDistanceMeters: number,
): boolean {
  const samplesA = a.connectionPointIndices.length > 0 ? a.connectionPointIndices : a.pointIndices;
  const samplesB = b.connectionPointIndices.length > 0 ? b.connectionPointIndices : b.pointIndices;
  const maximumSquared = maximumDistanceMeters * maximumDistanceMeters;
  for (const indexA of samplesA) {
    const offsetA = indexA * 3;
    const ax = Number(input.positions[offsetA]);
    const ay = Number(input.positions[offsetA + 1]);
    const az = Number(input.positions[offsetA + 2]);
    for (const indexB of samplesB) {
      const offsetB = indexB * 3;
      const dx = ax - Number(input.positions[offsetB]);
      const dy = ay - Number(input.positions[offsetB + 1]);
      const dz = az - Number(input.positions[offsetB + 2]);
      if (dx * dx + dy * dy + dz * dz <= maximumSquared) return true;
    }
  }
  return false;
}

function nodeSupportsConnect(
  a: OrientationNode,
  b: OrientationNode,
  input: SurfaceComponentInput,
  voxelSizeMeters: number,
  connectionCellSizeMeters: number,
): boolean {
  // Grid adjacency and AABB distance are broad phases only. Wide AABBs can overlap
  // even when all actual surfels live on opposite corners of their cells, so the
  // final link must be supported by bounded real point-to-point evidence.
  const maximumSupportGapMeters = clamp(voxelSizeMeters * 1.75, 0.045, 0.12);
  if (nodeSupportGapMeters(a, b) > maximumSupportGapMeters) return false;

  // The AABB test can also be fooled by one tail sample. A bounded centroid gate
  // cheaply rejects obviously separate families before the <=8x8 exact-support
  // check. Nodes themselves never span more than one hash cell.
  const centerA = nodeCenter(a);
  const centerB = nodeCenter(b);
  if (centerA === null || centerB === null) return false;
  const centerDistance = Math.hypot(
    centerA[0] - centerB[0],
    centerA[1] - centerB[1],
    centerA[2] - centerB[2],
  );
  if (centerDistance > connectionCellSizeMeters * 2.20) return false;
  return nodesHaveNearbyMeasuredSupport(a, b, input, maximumSupportGapMeters);
}

function compareSamples(left: CellSample, right: CellSample): number {
  return right.confidence - left.confidence ||
    left.normal[0] - right.normal[0] ||
    left.normal[1] - right.normal[1] ||
    left.normal[2] - right.normal[2] ||
    left.x - right.x ||
    left.y - right.y ||
    left.z - right.z;
}

function compareOrientationSupport(left: OrientationNode, right: OrientationNode): number {
  const leftNormal = nodeNormal(left) ?? [0, 0, 0];
  const rightNormal = nodeNormal(right) ?? [0, 0, 0];
  return right.pointIndices.length - left.pointIndices.length ||
    right.normalWeight - left.normalWeight ||
    leftNormal[0] - rightNormal[0] ||
    leftNormal[1] - rightNormal[1] ||
    leftNormal[2] - rightNormal[2];
}

function accumulateSample(node: OrientationNode, sample: CellSample): void {
  const reference = nodeNormal(node);
  const sign = reference !== null && (
    reference[0] * sample.normal[0] + reference[1] * sample.normal[1] + reference[2] * sample.normal[2]
  ) < 0 ? -1 : 1;
  const weight = clamp01(sample.confidence);
  node.normalX += sample.normal[0] * weight * sign;
  node.normalY += sample.normal[1] * weight * sign;
  node.normalZ += sample.normal[2] * weight * sign;
  node.normalWeight += weight;
  node.positionX += sample.x * weight;
  node.positionY += sample.y * weight;
  node.positionZ += sample.z * weight;
  node.positionWeight += weight;
  node.minX = Math.min(node.minX, sample.x);
  node.minY = Math.min(node.minY, sample.y);
  node.minZ = Math.min(node.minZ, sample.z);
  node.maxX = Math.max(node.maxX, sample.x);
  node.maxY = Math.max(node.maxY, sample.y);
  node.maxZ = Math.max(node.maxZ, sample.z);
  node.pointIndices.push(sample.index);
}

function buildNodeAdjacency(
  nodes: readonly OrientationNode[],
  nodesByCell: ReadonlyMap<string, readonly OrientationNode[]>,
  input: SurfaceComponentInput,
  voxelSizeMeters: number,
  connectionCellSizeMeters: number,
): number[][] {
  const adjacency: number[][] = Array.from({ length: nodes.length }, () => []);
  for (const current of nodes) {
    const currentNormal = nodeNormal(current);
    if (currentNormal === null) continue;
    for (let dx = -1; dx <= 1; dx += 1) {
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dz = -1; dz <= 1; dz += 1) {
          const neighbours = nodesByCell.get(key(current.gx + dx, current.gy + dy, current.gz + dz));
          if (neighbours === undefined) continue;
          for (const neighbour of neighbours) {
            if (neighbour.id <= current.id) continue;
            const neighbourNormal = nodeNormal(neighbour);
            // MAX-260: signed -- opposite-facing surfaces are different faces.
            if (neighbourNormal === null || signedDot(currentNormal, neighbourNormal) < COMPONENT_NORMAL_DOT) continue;
            if (!nodeSupportsConnect(current, neighbour, input, voxelSizeMeters, connectionCellSizeMeters)) continue;
            adjacency[current.id].push(neighbour.id);
            adjacency[neighbour.id].push(current.id);
          }
        }
      }
    }
  }
  for (const neighbours of adjacency) neighbours.sort((a, b) => a - b);
  return adjacency;
}

function nodeCanPropagate(
  nodeId: number,
  nodes: readonly OrientationNode[],
  adjacency: readonly (readonly number[])[],
): boolean {
  const node = nodes[nodeId];
  if (node.pointIndices.length >= MIN_NODE_PROPAGATION_POINTS) return true;
  let support = node.pointIndices.length;
  for (const neighbourId of adjacency[nodeId]) {
    support += Math.min(
      MAX_NEIGHBOUR_PROPAGATION_CONTRIBUTION,
      nodes[neighbourId].pointIndices.length,
    );
    if (support >= MIN_LOCAL_PROPAGATION_SUPPORT_POINTS) return true;
  }
  return false;
}

function meshThresholds(voxelSizeMeters: number): {
  major: number;
  minor: number;
  points: number;
  cells: number;
} {
  const voxel = clamp(Number.isFinite(voxelSizeMeters) ? voxelSizeMeters : 0.04, 0.025, 0.10);
  return {
    major: Math.max(0.14, voxel * 3.5),
    minor: Math.max(0.075, voxel * 1.85),
    points: MESH_MIN_POINTS,
    cells: MESH_MIN_OCCUPIED_CELLS,
  };
}

function readinessRatio(value: number, target: number): number {
  return target <= 0 ? 1 : clamp01(value / target);
}

function robustProjectedExtent(
  pointIndices: readonly number[],
  input: SurfaceComponentInput,
  center: SurfaceComponentPoint,
  axis: SurfaceComponentNormal,
  cellSizeMeters: number,
): number {
  if (pointIndices.length <= 1) return cellSizeMeters;
  const projected = new Array<number>(pointIndices.length);
  for (let cursor = 0; cursor < pointIndices.length; cursor += 1) {
    const offset = pointIndices[cursor] * 3;
    projected[cursor] =
      (Number(input.positions[offset]) - center[0]) * axis[0] +
      (Number(input.positions[offset + 1]) - center[1]) * axis[1] +
      (Number(input.positions[offset + 2]) - center[2]) * axis[2];
  }
  projected.sort((a, b) => a - b);
  const trim = pointIndices.length >= MIN_POINTS_FOR_ROBUST_EXTENT
    ? Math.min(
      Math.floor((pointIndices.length - 1) * ROBUST_EXTENT_TRIM_FRACTION),
      Math.floor((pointIndices.length - 2) / 2),
    )
    : 0;
  const low = projected[trim];
  const high = projected[projected.length - 1 - trim];
  return Math.max(cellSizeMeters, Number.isFinite(high - low) ? high - low : 0);
}

/**
 * MAX-254: planar area of a component. PCA extents are unusable for this: on a
 * square face the in-plane eigenvalues are equal, the axes may lie along the
 * diagonals and major x minor then doubles the area. Instead take the convex
 * hull of the points in the plane (independent of the axis choice) and add the
 * half-spacing border strip that sampled points cannot reach.
 */
function planarAreaEstimate(
  pointIndices: readonly number[],
  input: SurfaceComponentInput,
  center: SurfaceComponentPoint,
  axisU: SurfaceComponentNormal,
  axisV: SurfaceComponentNormal,
): number {
  const n = pointIndices.length;
  if (n < 3) return 0;
  const points: Array<[number, number]> = pointIndices.map((index) => {
    const offset = index * 3;
    const dx = Number(input.positions[offset]) - center[0];
    const dy = Number(input.positions[offset + 1]) - center[1];
    const dz = Number(input.positions[offset + 2]) - center[2];
    return [dx * axisU[0] + dy * axisU[1] + dz * axisU[2], dx * axisV[0] + dy * axisV[1] + dz * axisV[2]];
  });
  points.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cross = (o: [number, number], a: [number, number], b: [number, number]): number =>
    (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const lower: Array<[number, number]> = [];
  for (const point of points) {
    while (lower.length >= 2 && cross(lower[lower.length - 2], lower[lower.length - 1], point) <= 0) lower.pop();
    lower.push(point);
  }
  const upper: Array<[number, number]> = [];
  for (let i = points.length - 1; i >= 0; i -= 1) {
    const point = points[i];
    while (upper.length >= 2 && cross(upper[upper.length - 2], upper[upper.length - 1], point) <= 0) upper.pop();
    upper.push(point);
  }
  const hull = [...lower.slice(0, -1), ...upper.slice(0, -1)];
  let twiceArea = 0; let perimeter = 0;
  for (let i = 0; i < hull.length; i += 1) {
    const a = hull[i]; const b = hull[(i + 1) % hull.length];
    twiceArea += a[0] * b[1] - b[0] * a[1];
    perimeter += Math.hypot(b[0] - a[0], b[1] - a[1]);
  }
  const hullArea = Math.abs(twiceArea) / 2;
  const spacing = Math.sqrt(hullArea / n);
  return hullArea + perimeter * spacing * 0.5 + spacing * spacing;
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

  // Covariance gives stable principal directions, but sqrt(variance) alone is
  // not a robust physical size estimate: a few connected flying/tail surfels can
  // inflate it substantially. Measure the central projected range on each PCA
  // axis instead. With >=20 points, trimming 10% from both ends removes sparse
  // tails while preserving full extents of ordinary sampled grids (each boundary
  // row/column contributes many points). The cell size remains a conservative
  // lower bound for unresolved dimensions.
  const thicknessMeters = robustProjectedExtent(pointIndices, input, center, eigen.vectors[0], cellSizeMeters);
  const minorExtentMeters = robustProjectedExtent(pointIndices, input, center, eigen.vectors[1], cellSizeMeters);
  const majorExtentMeters = robustProjectedExtent(pointIndices, input, center, eigen.vectors[2], cellSizeMeters);
  const expectedSurfaceCells = Math.max(
    1,
    Math.ceil(majorExtentMeters / cellSizeMeters) * Math.ceil(minorExtentMeters / cellSizeMeters),
  );
  const occupancyRatio = clamp01(occupiedCellCount / expectedSurfaceCells);
  const thresholds = meshThresholds(input.voxelSizeMeters);
  const estimatedAreaSquareMeters = planarAreaEstimate(pointIndices, input, center, eigen.vectors[1], eigen.vectors[2]);
  const normalLength = Math.hypot(eigen.vectors[0][0], eigen.vectors[0][1], eigen.vectors[0][2]);
  const normal: SurfaceComponentNormal = normalLength > 1e-9
    ? [eigen.vectors[0][0] / normalLength, eigen.vectors[0][1] / normalLength, eigen.vectors[0][2] / normalLength]
    : [0, 0, 1];
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
    normal,
    estimatedAreaSquareMeters,
  };
}

/**
 * Memory-bounded connected-component analysis for unordered measured surfels.
 *
 * We intentionally do not run all-point DBSCAN on the phone. Points first enter
 * a voxel hash. Each occupied cell may form at most twelve bounded provisional
 * normal families; they are ranked deterministically by measured support and only
 * the strongest three survive. A bounded adjacency graph then visits only 3x3x3
 * neighbouring cells. Grid adjacency, AABB gap and centroid range are broad phases;
 * compatible families must still have real measured support within the metric link
 * radius, using at most 8x8 preselected real surfels. Sparse nodes may attach as
 * leaves, but only locally supported nodes propagate component growth. This keeps
 * smooth/convex measured surfaces connected without allowing a 1-point gradual-
 * normal chain to bridge otherwise separate objects.
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
  const bucketsByCell = new Map<string, CellBucket>();

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
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z) || rawNormal === null) continue;
    const normal = rawNormal;
    const gx = grid(x, connectionCellSizeMeters);
    const gy = grid(y, connectionCellSizeMeters);
    const gz = grid(z, connectionCellSizeMeters);
    const spatialKey = key(gx, gy, gz);
    let bucket = bucketsByCell.get(spatialKey);
    if (bucket === undefined) {
      bucket = { gx, gy, gz, spatialKey, samples: [] };
      bucketsByCell.set(spatialKey, bucket);
    }
    bucket.samples.push({ index, x, y, z, confidence, normal });
  }

  const nodes: OrientationNode[] = [];
  const nodesByCell = new Map<string, OrientationNode[]>();
  let acceptedPointCount = 0;
  const orderedBuckets = [...bucketsByCell.values()].sort((left, right) =>
    left.gx - right.gx || left.gy - right.gy || left.gz - right.gz
  );

  for (const bucket of orderedBuckets) {
    bucket.samples.sort(compareSamples);
    const provisional: OrientationNode[] = [];
    for (const sample of bucket.samples) {
      let best: OrientationNode | null = null;
      let bestDot = ORIENTATION_FAMILY_DOT;
      for (const node of provisional) {
        const reference = nodeNormal(node);
        if (reference === null) continue;
        const similarity = signedDot(reference, sample.normal);
        if (similarity >= bestDot) { best = node; bestDot = similarity; }
      }
      if (best === null) {
        if (provisional.length >= MAX_PROVISIONAL_ORIENTATION_NODES_PER_CELL) continue;
        best = {
          id: -1,
          gx: bucket.gx,
          gy: bucket.gy,
          gz: bucket.gz,
          spatialKey: bucket.spatialKey,
          pointIndices: [],
          connectionPointIndices: [],
          normalX: 0,
          normalY: 0,
          normalZ: 0,
          normalWeight: 0,
          positionX: 0,
          positionY: 0,
          positionZ: 0,
          positionWeight: 0,
          minX: Number.POSITIVE_INFINITY,
          minY: Number.POSITIVE_INFINITY,
          minZ: Number.POSITIVE_INFINITY,
          maxX: Number.NEGATIVE_INFINITY,
          maxY: Number.NEGATIVE_INFINITY,
          maxZ: Number.NEGATIVE_INFINITY,
        };
        provisional.push(best);
      }
      accumulateSample(best, sample);
    }

    const retained = provisional
      .sort(compareOrientationSupport)
      .slice(0, MAX_ORIENTATION_NODES_PER_CELL);
    for (const node of retained) {
      node.id = nodes.length;
      node.connectionPointIndices = selectConnectionPointIndices(node, input);
      nodes.push(node);
      acceptedPointCount += node.pointIndices.length;
    }
    if (retained.length > 0) nodesByCell.set(bucket.spatialKey, retained);
  }

  const adjacency = buildNodeAdjacency(nodes, nodesByCell, input, voxel, connectionCellSizeMeters);
  const propagationReady = new Uint8Array(nodes.length);
  for (const node of nodes) {
    if (nodeCanPropagate(node.id, nodes, adjacency)) propagationReady[node.id] = 1;
  }
  // Strong/local-supported nodes seed components first so a weak bridge node is
  // claimed as a leaf by a real surface rather than becoming an order-dependent
  // relay before that surface is visited.
  const traversalSeeds = [...nodes].sort((left, right) =>
    propagationReady[right.id] - propagationReady[left.id] ||
    right.pointIndices.length - left.pointIndices.length ||
    left.id - right.id
  );
  const visited = new Uint8Array(nodes.length);
  const components: SurfaceComponent[] = [];
  const componentPointIndices: number[][] = [];

  for (const seed of traversalSeeds) {
    if (visited[seed.id] !== 0) continue;
    visited[seed.id] = 1;
    const queue: OrientationNode[] = [seed];
    const points: number[] = [];
    const occupiedCells = new Set<string>();
    for (let read = 0; read < queue.length; read += 1) {
      const current = queue[read];
      points.push(...current.pointIndices);
      occupiedCells.add(current.spatialKey);
      if (propagationReady[current.id] === 0) continue;
      for (const neighbourId of adjacency[current.id]) {
        if (visited[neighbourId] !== 0) continue;
        visited[neighbourId] = 1;
        queue.push(nodes[neighbourId]);
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
  const componentIdByPoint = new Int32Array(sourceCount);
  componentIdByPoint.fill(-1);
  let eligiblePointCount = 0;
  let eligibleComponentCount = 0;
  for (let componentIndex = 0; componentIndex < components.length; componentIndex += 1) {
    const component = components[componentIndex];
    for (const pointIndex of componentPointIndices[componentIndex]) componentIdByPoint[pointIndex] = component.id;
    if (!component.meshEligible) continue;
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
    componentIdByPoint,
    components,
    largestComponent: bySize[0] ?? null,
    bestMeshComponent: byMesh[0] ?? null,
    connectionCellSizeMeters,
  };
}
