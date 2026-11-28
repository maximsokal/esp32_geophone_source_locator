import type { MeshVertexKey } from './meshKeys';

export type MeshNormalIntrusionPoint = readonly [number, number, number];

export interface MeshNormalIntrusionTriangle {
  vertices: readonly [MeshNormalIntrusionPoint, MeshNormalIntrusionPoint, MeshNormalIntrusionPoint];
  keys: readonly [MeshVertexKey, MeshVertexKey, MeshVertexKey];
}

export interface MeshNormalIntrusionComponentStats {
  intrudingTriangleCount: number;
  intrudingTriangleRatio: number;
}

export interface MeshNormalIntrusionDiagnostic {
  candidatePairs: number;
  exactTestedPairs: number;
  intrudingTriangleCount: number;
  truncated: boolean;
  oversizedTriangleCount: number;
  triangleIntrusionHits: Uint8Array;
  componentStats: readonly MeshNormalIntrusionComponentStats[];
}

export const MAX_NORMAL_INTRUSION_CANDIDATE_PAIRS = 40_000;
export const MAX_NORMAL_INTRUSION_EXACT_TESTS = 8_000;
export const MAX_NORMAL_INTRUSION_CELLS_PER_TRIANGLE = 64;
/** Budgets grow with the mesh, for the same reason as the self-intersection ones: a fixed budget ran out on a large clean mesh and withheld all of it. */
const CANDIDATE_PAIRS_PER_TRIANGLE = 8;
const EXACT_TESTS_PER_TRIANGLE = 2;
export const MAX_NORMAL_INTRUSION_CANDIDATE_PAIRS_CEILING = 400_000;
export const MAX_NORMAL_INTRUSION_EXACT_TESTS_CEILING = 100_000;
export function normalIntrusionBudgets(triangleCount: number): { candidatePairs: number; exactTests: number } {
  return {
    candidatePairs: Math.min(MAX_NORMAL_INTRUSION_CANDIDATE_PAIRS_CEILING, Math.max(MAX_NORMAL_INTRUSION_CANDIDATE_PAIRS, triangleCount * CANDIDATE_PAIRS_PER_TRIANGLE)),
    exactTests: Math.min(MAX_NORMAL_INTRUSION_EXACT_TESTS_CEILING, Math.max(MAX_NORMAL_INTRUSION_EXACT_TESTS, triangleCount * EXACT_TESTS_PER_TRIANGLE)),
  };
}
const MAX_OVERSIZED_TARGETS = 128;
const BARYCENTRIC_EPSILON = 1e-6;

type Point = MeshNormalIntrusionPoint;

interface Aabb {
  minX: number;
  minY: number;
  minZ: number;
  maxX: number;
  maxY: number;
  maxZ: number;
}

interface PreparedTriangle {
  triangle: MeshNormalIntrusionTriangle;
  centroid: Point;
  normal: Point;
  aabb: Aabb;
  priority: number;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function subtract(a: Point, b: Point): Point {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function cross(a: Point, b: Point): Point {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function dot(a: Point, b: Point): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function normalize(vector: Point): Point | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return Number.isFinite(magnitude) && magnitude > 1e-12
    ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude]
    : null;
}

function centroid(triangle: MeshNormalIntrusionTriangle): Point {
  const [a, b, c] = triangle.vertices;
  return [
    (a[0] + b[0] + c[0]) / 3,
    (a[1] + b[1] + c[1]) / 3,
    (a[2] + b[2] + c[2]) / 3,
  ];
}

function triangleNormal(triangle: MeshNormalIntrusionTriangle): Point | null {
  return normalize(cross(
    subtract(triangle.vertices[1], triangle.vertices[0]),
    subtract(triangle.vertices[2], triangle.vertices[0]),
  ));
}

function triangleAabb(triangle: MeshNormalIntrusionTriangle): Aabb {
  const [a, b, c] = triangle.vertices;
  return {
    minX: Math.min(a[0], b[0], c[0]),
    minY: Math.min(a[1], b[1], c[1]),
    minZ: Math.min(a[2], b[2], c[2]),
    maxX: Math.max(a[0], b[0], c[0]),
    maxY: Math.max(a[1], b[1], c[1]),
    maxZ: Math.max(a[2], b[2], c[2]),
  };
}

function sharedVertex(a: MeshNormalIntrusionTriangle, b: MeshNormalIntrusionTriangle): boolean {
  return a.keys[0] === b.keys[0] || a.keys[0] === b.keys[1] || a.keys[0] === b.keys[2] ||
    a.keys[1] === b.keys[0] || a.keys[1] === b.keys[1] || a.keys[1] === b.keys[2] ||
    a.keys[2] === b.keys[0] || a.keys[2] === b.keys[1] || a.keys[2] === b.keys[2];
}

function grid(value: number, cellSizeMeters: number): number {
  return Math.floor(value / cellSizeMeters);
}

/** one number per grid cell: exact for cell indices within +-65536 (kilometres at the smallest cell) */
function cellKey(x: number, y: number, z: number): number {
  return ((x + 65536) * 131072 + (y + 65536)) * 131072 + (z + 65536);
}

function aabbCellBounds(aabb: Aabb, cellSizeMeters: number): readonly [number, number, number, number, number, number] {
  return [
    grid(aabb.minX, cellSizeMeters), grid(aabb.maxX, cellSizeMeters),
    grid(aabb.minY, cellSizeMeters), grid(aabb.maxY, cellSizeMeters),
    grid(aabb.minZ, cellSizeMeters), grid(aabb.maxZ, cellSizeMeters),
  ];
}

function cellCount(bounds: readonly [number, number, number, number, number, number]): number {
  return (bounds[1] - bounds[0] + 1) * (bounds[3] - bounds[2] + 1) * (bounds[5] - bounds[4] + 1);
}

function segmentIntersectsAabb(start: Point, end: Point, aabb: Aabb, epsilon: number): boolean {
  let tMin = 0;
  let tMax = 1;
  for (let axis = 0; axis < 3; axis += 1) {
    const origin = start[axis];
    const direction = end[axis] - start[axis];
    const minimum = axis === 0 ? aabb.minX : axis === 1 ? aabb.minY : aabb.minZ;
    const maximum = axis === 0 ? aabb.maxX : axis === 1 ? aabb.maxY : aabb.maxZ;
    if (Math.abs(direction) <= 1e-12) {
      if (origin < minimum - epsilon || origin > maximum + epsilon) return false;
      continue;
    }
    let first = (minimum - epsilon - origin) / direction;
    let second = (maximum + epsilon - origin) / direction;
    if (first > second) [first, second] = [second, first];
    tMin = Math.max(tMin, first);
    tMax = Math.min(tMax, second);
    if (tMin > tMax) return false;
  }
  return true;
}

/** Moller-Trumbore in scalars: it runs for every exact test, so it makes no temporary vectors. */
function segmentIntersectsTriangle(
  start: Point,
  end: Point,
  triangle: MeshNormalIntrusionTriangle,
): boolean {
  const a = triangle.vertices[0], b = triangle.vertices[1], c = triangle.vertices[2];
  const dx = end[0] - start[0], dy = end[1] - start[1], dz = end[2] - start[2];
  const e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
  const e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
  const hx = dy * e2z - dz * e2y, hy = dz * e2x - dx * e2z, hz = dx * e2y - dy * e2x;
  const determinant = e1x * hx + e1y * hy + e1z * hz;
  if (Math.abs(determinant) <= 1e-12) return false;
  const inverse = 1 / determinant;
  const sx = start[0] - a[0], sy = start[1] - a[1], sz = start[2] - a[2];
  const u = inverse * (sx * hx + sy * hy + sz * hz);
  if (u < -BARYCENTRIC_EPSILON || u > 1 + BARYCENTRIC_EPSILON) return false;
  const qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
  const v = inverse * (dx * qx + dy * qy + dz * qz);
  if (v < -BARYCENTRIC_EPSILON || u + v > 1 + BARYCENTRIC_EPSILON) return false;
  const t = inverse * (e2x * qx + e2y * qy + e2z * qz);
  return t >= BARYCENTRIC_EPSILON && t <= 1 + BARYCENTRIC_EPSILON;
}

function stablePriority(prepared: Omit<PreparedTriangle, 'priority'>, cellSizeMeters: number): number {
  const quantization = Math.max(cellSizeMeters / 16, 1e-5);
  const values = [
    Math.round(prepared.centroid[0] / quantization),
    Math.round(prepared.centroid[1] / quantization),
    Math.round(prepared.centroid[2] / quantization),
    Math.round(prepared.normal[0] * 1024),
    Math.round(prepared.normal[1] * 1024),
    Math.round(prepared.normal[2] * 1024),
  ];
  let hash = 0x811c9dc5;
  for (const value of values) hash = Math.imul(hash ^ value, 0x01000193);
  hash ^= hash >>> 16;
  return hash >>> 0;
}

/**
 * Bounded diagnostic for local mesh folding/double-shell noise.
 *
 * A short segment is cast in both normal directions from each triangle centroid.
 * Adjacent faces sharing a quantized vertex are ignored. A spatial AABB hash
 * supplies local candidates and both candidate and exact-test counts are hard
 * capped, so the pass cannot degrade to O(T^2) on a phone.
 *
 * A hit is evidence only. Legitimate thin two-sided surfaces can also be close
 * along their normals, therefore callers must combine this ratio with topology,
 * coverage or self-intersection evidence instead of deleting geometry from one
 * normal hit alone.
 */
export function diagnoseMeshNormalIntrusions(
  triangles: readonly MeshNormalIntrusionTriangle[],
  componentByTriangle: Int32Array,
  componentCount: number,
  voxelSizeMeters: number,
): MeshNormalIntrusionDiagnostic {
  const safeVoxel = clamp(Math.abs(voxelSizeMeters), 0.01, 0.12);
  const cellSizeMeters = clamp(safeVoxel * 2.0, 0.06, 0.18);
  const rayLengthMeters = clamp(safeVoxel * 1.75, 0.05, 0.16);
  const startOffsetMeters = clamp(safeVoxel * 0.08, 0.0015, 0.006);
  const metricEpsilon = clamp(safeVoxel * 0.015, 1e-5, 8e-4);

  const prepared: PreparedTriangle[] = [];
  for (const triangle of triangles) {
    const normal = triangleNormal(triangle);
    if (normal === null) {
      prepared.push({
        triangle,
        centroid: centroid(triangle),
        normal: [0, 0, 0],
        aabb: triangleAabb(triangle),
        priority: 0xffffffff,
      });
      continue;
    }
    const partial = {
      triangle,
      centroid: centroid(triangle),
      normal,
      aabb: triangleAabb(triangle),
    };
    prepared.push({ ...partial, priority: stablePriority(partial, cellSizeMeters) });
  }

  const gridBuckets = new Map<number, number[]>();
  const oversized: number[] = [];
  let oversizedTriangleCount = 0;
  for (let index = 0; index < prepared.length; index += 1) {
    const bounds = aabbCellBounds(prepared[index].aabb, cellSizeMeters);
    if (cellCount(bounds) > MAX_NORMAL_INTRUSION_CELLS_PER_TRIANGLE) {
      oversizedTriangleCount += 1;
      if (oversized.length < MAX_OVERSIZED_TARGETS) oversized.push(index);
      continue;
    }
    for (let gx = bounds[0]; gx <= bounds[1]; gx += 1) {
      for (let gy = bounds[2]; gy <= bounds[3]; gy += 1) {
        for (let gz = bounds[4]; gz <= bounds[5]; gz += 1) {
          const bucketKey = cellKey(gx, gy, gz);
          const bucket = gridBuckets.get(bucketKey);
          if (bucket === undefined) gridBuckets.set(bucketKey, [index]);
          else bucket.push(index);
        }
      }
    }
  }

  // Each box widened by twice the tolerance (min x, y, z, max x, y, z). A ray wholly outside it cannot pass the
  // slab test, so most bucket entries are dropped by six comparisons with no rounding question.
  const reach = new Float64Array(prepared.length * 6);
  for (let index = 0; index < prepared.length; index += 1) {
    const box = prepared[index].aabb;
    reach[index * 6] = box.minX - 2 * metricEpsilon;
    reach[index * 6 + 1] = box.minY - 2 * metricEpsilon;
    reach[index * 6 + 2] = box.minZ - 2 * metricEpsilon;
    reach[index * 6 + 3] = box.maxX + 2 * metricEpsilon;
    reach[index * 6 + 4] = box.maxY + 2 * metricEpsilon;
    reach[index * 6 + 5] = box.maxZ + 2 * metricEpsilon;
  }

  const triangleIntrusionHits = new Uint8Array(triangles.length);
  const order = prepared
    .map((triangle, index) => ({ index, priority: triangle.priority }))
    .sort((left, right) => left.priority - right.priority || left.index - right.index);
  const budgets = normalIntrusionBudgets(triangles.length);
  let candidatePairs = 0;
  let exactTestedPairs = 0;
  let truncated = false;

  // Scratch state of the query, made once: visited[target] holds the number of the ray that last saw the target.
  const visited = new Int32Array(prepared.length);
  let rayNumber = 0;
  const lists: (readonly number[])[] = [];
  const start: [number, number, number] = [0, 0, 0];
  const end: [number, number, number] = [0, 0, 0];

  outer: for (const source of order) {
    const preparedSource = prepared[source.index];
    const { centroid: origin, normal } = preparedSource;
    if (normal[0] === 0 && normal[1] === 0 && normal[2] === 0) continue;
    for (const sign of [-1, 1] as const) {
      for (let axis = 0; axis < 3; axis += 1) {
        start[axis] = origin[axis] + normal[axis] * startOffsetMeters * sign;
        end[axis] = origin[axis] + normal[axis] * rayLengthMeters * sign;
      }
      const lowX = Math.min(start[0], end[0]), highX = Math.max(start[0], end[0]);
      const lowY = Math.min(start[1], end[1]), highY = Math.max(start[1], end[1]);
      const lowZ = Math.min(start[2], end[2]), highZ = Math.max(start[2], end[2]);
      const maxX = grid(highX, cellSizeMeters);
      const maxY = grid(highY, cellSizeMeters);
      const maxZ = grid(highZ, cellSizeMeters);
      const minY = grid(lowY, cellSizeMeters);
      const minZ = grid(lowZ, cellSizeMeters);
      lists.length = 0;
      for (let gx = grid(lowX, cellSizeMeters); gx <= maxX; gx += 1) {
        for (let gy = minY; gy <= maxY; gy += 1) {
          for (let gz = minZ; gz <= maxZ; gz += 1) {
            const bucket = gridBuckets.get(cellKey(gx, gy, gz));
            if (bucket !== undefined) lists.push(bucket);
          }
        }
      }
      lists.push(oversized);

      // Candidates are tested while the buckets are walked, each at its first appearance, so the order (and with
      // it what a budget cuts off) is fixed by the grid, and a hit ends the walk.
      rayNumber += 1;
      for (const list of lists) {
        for (const target of list) {
          if (visited[target] === rayNumber) continue;
          visited[target] = rayNumber;
          if (target === source.index) continue;
          const at = target * 6;
          if (
            highX < reach[at] || lowX > reach[at + 3] ||
            highY < reach[at + 1] || lowY > reach[at + 4] ||
            highZ < reach[at + 2] || lowZ > reach[at + 5]
          ) continue;
          const preparedTarget = prepared[target];
          if (!segmentIntersectsAabb(start, end, preparedTarget.aabb, metricEpsilon)) continue;
          if (sharedVertex(preparedSource.triangle, preparedTarget.triangle)) continue;
          candidatePairs += 1;
          if (candidatePairs > budgets.candidatePairs) {
            truncated = true;
            break outer;
          }
          if (exactTestedPairs >= budgets.exactTests) {
            truncated = true;
            break outer;
          }
          exactTestedPairs += 1;
          if (!segmentIntersectsTriangle(start, end, preparedTarget.triangle)) continue;
          triangleIntrusionHits[source.index] = 1;
          continue outer;
        }
      }
    }
  }

  const componentSizes = new Uint32Array(Math.max(0, componentCount));
  const componentHits = new Uint32Array(Math.max(0, componentCount));
  let intrudingTriangleCount = 0;
  for (let index = 0; index < triangles.length; index += 1) {
    const component = index < componentByTriangle.length ? componentByTriangle[index] : -1;
    if (component >= 0 && component < componentSizes.length) componentSizes[component] += 1;
    if (triangleIntrusionHits[index] === 0) continue;
    intrudingTriangleCount += 1;
    if (component >= 0 && component < componentHits.length) componentHits[component] += 1;
  }

  const componentStats: MeshNormalIntrusionComponentStats[] = Array.from(
    { length: componentSizes.length },
    (_, component) => ({
      intrudingTriangleCount: componentHits[component],
      intrudingTriangleRatio: componentSizes[component] > 0
        ? componentHits[component] / componentSizes[component]
        : 0,
    }),
  );

  return {
    candidatePairs: Math.min(candidatePairs, budgets.candidatePairs),
    exactTestedPairs,
    intrudingTriangleCount,
    truncated,
    oversizedTriangleCount,
    triangleIntrusionHits,
    componentStats,
  };
}
