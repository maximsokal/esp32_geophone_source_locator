export type MeshIntersectionPoint = readonly [number, number, number];

/** Equal keys mean the same mesh vertex: that is what makes two triangles neighbours. */
export type MeshVertexKey = string | number;
export interface MeshIntersectionTriangle {
  vertices: readonly [MeshIntersectionPoint, MeshIntersectionPoint, MeshIntersectionPoint];
  keys: readonly [MeshVertexKey, MeshVertexKey, MeshVertexKey];
}

export interface MeshSelfIntersectionComponentStats {
  intersectingPairCount: number;
  affectedTriangleCount: number;
  affectedTriangleRatio: number;
}

export interface MeshSelfIntersectionDiagnostic {
  broadPhaseCandidatePairs: number;
  exactTestedPairs: number;
  intersectingPairCount: number;
  affectedTriangleCount: number;
  truncated: boolean;
  oversizedTriangleCount: number;
  triangleIntersectionHits: Uint16Array;
  componentStats: readonly MeshSelfIntersectionComponentStats[];
}

/**
 * Candidate pairs come from a uniform grid over the boxes (widened by the tolerance); a triangle spanning too many cells is compared with every other one,
 * so no pair is missed and no triangle is "too big". three-mesh-bvh checks this in the tests; it is not shipped, because every TSDF worker would carry its
 * own copy of it and of three (+191 kB gzip of ESP32 assets, measured). Only the number of EXACT tests is capped,
 * against a pathological soup in which everything overlaps everything: it grows with the mesh (a manifold mesh needs about a dozen per triangle) and a
 * result that hit it says so (`truncated`).
 */
export const MAX_SELF_INTERSECTION_EXACT_TESTS = 12_000;
export const MAX_SELF_INTERSECTION_EXACT_TESTS_CEILING = 600_000;
const EXACT_TESTS_PER_TRIANGLE = 32;
/** A triangle spanning more grid cells than this is compared directly with the others. */
export const MAX_SELF_INTERSECTION_CELLS_PER_TRIANGLE = 64;
/** Ceiling of those direct comparisons, reached only by a soup made mostly of huge triangles; the result then says it is truncated. */
export const MAX_SELF_INTERSECTION_LARGE_COMPARISONS = 4_000_000;
export function selfIntersectionBudgets(triangleCount: number): { exactTests: number } {
  return { exactTests: Math.min(MAX_SELF_INTERSECTION_EXACT_TESTS_CEILING, Math.max(MAX_SELF_INTERSECTION_EXACT_TESTS, triangleCount * EXACT_TESTS_PER_TRIANGLE)) };
}
const BARYCENTRIC_EPSILON = 1e-6;

interface Aabb {
  minX: number;
  minY: number;
  minZ: number;
  maxX: number;
  maxY: number;
  maxZ: number;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function subtract(a: MeshIntersectionPoint, b: MeshIntersectionPoint): MeshIntersectionPoint {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function cross(a: MeshIntersectionPoint, b: MeshIntersectionPoint): MeshIntersectionPoint {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function dot(a: MeshIntersectionPoint, b: MeshIntersectionPoint): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function aabbForTriangle(triangle: MeshIntersectionTriangle): Aabb {
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

function aabbOverlap(a: Aabb, b: Aabb, epsilon: number): boolean {
  return !(
    a.maxX < b.minX - epsilon || b.maxX < a.minX - epsilon ||
    a.maxY < b.minY - epsilon || b.maxY < a.minY - epsilon ||
    a.maxZ < b.minZ - epsilon || b.maxZ < a.minZ - epsilon
  );
}

function dominantAxis(normal: MeshIntersectionPoint): 0 | 1 | 2 {
  const ax = Math.abs(normal[0]);
  const ay = Math.abs(normal[1]);
  const az = Math.abs(normal[2]);
  return ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
}

function project2(point: MeshIntersectionPoint, dropAxis: 0 | 1 | 2): readonly [number, number] {
  if (dropAxis === 0) return [point[1], point[2]];
  if (dropAxis === 1) return [point[0], point[2]];
  return [point[0], point[1]];
}

function intervalsOverlap(
  first: readonly [number, number],
  second: readonly [number, number],
  epsilon: number,
): boolean {
  return first[1] >= second[0] - epsilon && second[1] >= first[0] - epsilon;
}

function projectionInterval(
  triangle: readonly [readonly [number, number], readonly [number, number], readonly [number, number]],
  axisX: number,
  axisY: number,
): readonly [number, number] {
  const p0 = triangle[0][0] * axisX + triangle[0][1] * axisY;
  const p1 = triangle[1][0] * axisX + triangle[1][1] * axisY;
  const p2 = triangle[2][0] * axisX + triangle[2][1] * axisY;
  return [Math.min(p0, p1, p2), Math.max(p0, p1, p2)];
}

/** Convex 2-D SAT used only for genuinely coplanar 3-D triangles. */
function coplanarTrianglesOverlap(
  a: MeshIntersectionTriangle,
  b: MeshIntersectionTriangle,
  normal: MeshIntersectionPoint,
  epsilonMeters: number,
): boolean {
  const dropAxis = dominantAxis(normal);
  const ta = [project2(a.vertices[0], dropAxis), project2(a.vertices[1], dropAxis), project2(a.vertices[2], dropAxis)] as const;
  const tb = [project2(b.vertices[0], dropAxis), project2(b.vertices[1], dropAxis), project2(b.vertices[2], dropAxis)] as const;
  for (const triangle of [ta, tb] as const) {
    for (let edge = 0; edge < 3; edge += 1) {
      const start = triangle[edge];
      const end = triangle[(edge + 1) % 3];
      const dx = end[0] - start[0];
      const dy = end[1] - start[1];
      const axisLength = Math.hypot(dx, dy);
      if (axisLength <= 1e-12) continue;
      const axisX = -dy / axisLength;
      const axisY = dx / axisLength;
      if (!intervalsOverlap(
        projectionInterval(ta, axisX, axisY),
        projectionInterval(tb, axisX, axisY),
        epsilonMeters,
      )) return false;
    }
  }
  return true;
}

function segmentIntersectsTriangle(
  start: MeshIntersectionPoint,
  end: MeshIntersectionPoint,
  triangle: MeshIntersectionTriangle,
): boolean {
  const direction = subtract(end, start);
  const edge1 = subtract(triangle.vertices[1], triangle.vertices[0]);
  const edge2 = subtract(triangle.vertices[2], triangle.vertices[0]);
  const h = cross(direction, edge2);
  const determinant = dot(edge1, h);
  if (Math.abs(determinant) <= 1e-12) return false;
  const inverse = 1 / determinant;
  const s = subtract(start, triangle.vertices[0]);
  const u = inverse * dot(s, h);
  if (u < -BARYCENTRIC_EPSILON || u > 1 + BARYCENTRIC_EPSILON) return false;
  const q = cross(s, edge1);
  const v = inverse * dot(direction, q);
  if (v < -BARYCENTRIC_EPSILON || u + v > 1 + BARYCENTRIC_EPSILON) return false;
  const t = inverse * dot(edge2, q);
  return t >= -BARYCENTRIC_EPSILON && t <= 1 + BARYCENTRIC_EPSILON;
}

function trianglePlaneSideRejected(
  triangle: MeshIntersectionTriangle,
  planeOrigin: MeshIntersectionPoint,
  planeNormal: MeshIntersectionPoint,
  normalLength: number,
  epsilonMeters: number,
): boolean {
  let positive = 0;
  let negative = 0;
  for (const point of triangle.vertices) {
    const distance = dot(subtract(point, planeOrigin), planeNormal) / normalLength;
    if (distance > epsilonMeters) positive += 1;
    else if (distance < -epsilonMeters) negative += 1;
  }
  return positive === 3 || negative === 3;
}

export function trianglesIntersect(
  a: MeshIntersectionTriangle,
  b: MeshIntersectionTriangle,
  epsilonMeters: number,
): boolean {
  const aEdge1 = subtract(a.vertices[1], a.vertices[0]);
  const aEdge2 = subtract(a.vertices[2], a.vertices[0]);
  const bEdge1 = subtract(b.vertices[1], b.vertices[0]);
  const bEdge2 = subtract(b.vertices[2], b.vertices[0]);
  const normalA = cross(aEdge1, aEdge2);
  const normalB = cross(bEdge1, bEdge2);
  const lengthA = Math.hypot(normalA[0], normalA[1], normalA[2]);
  const lengthB = Math.hypot(normalB[0], normalB[1], normalB[2]);
  if (lengthA <= 1e-12 || lengthB <= 1e-12) return false;
  const metricEpsilon = clamp(Math.abs(epsilonMeters), 1e-7, 0.002);

  if (trianglePlaneSideRejected(b, a.vertices[0], normalA, lengthA, metricEpsilon)) return false;
  if (trianglePlaneSideRejected(a, b.vertices[0], normalB, lengthB, metricEpsilon)) return false;

  const normalCross = cross(normalA, normalB);
  const sine = Math.hypot(normalCross[0], normalCross[1], normalCross[2]) / (lengthA * lengthB);
  if (sine <= 1e-5) {
    const maximumPlaneDistance = Math.max(
      ...b.vertices.map((point) => Math.abs(dot(subtract(point, a.vertices[0]), normalA)) / lengthA),
    );
    return maximumPlaneDistance <= metricEpsilon && coplanarTrianglesOverlap(a, b, normalA, metricEpsilon);
  }

  for (let edge = 0; edge < 3; edge += 1) {
    if (segmentIntersectsTriangle(a.vertices[edge], a.vertices[(edge + 1) % 3], b)) return true;
    if (segmentIntersectsTriangle(b.vertices[edge], b.vertices[(edge + 1) % 3], a)) return true;
  }
  return false;
}

/** A segment that crosses the INTERIOR of a triangle: touching its border or ending on it does not count (used between neighbours, which touch by design). */
function segmentPiercesTriangle(start: MeshIntersectionPoint, end: MeshIntersectionPoint, triangle: MeshIntersectionTriangle): boolean {
  const direction = subtract(end, start);
  const edge1 = subtract(triangle.vertices[1], triangle.vertices[0]);
  const edge2 = subtract(triangle.vertices[2], triangle.vertices[0]);
  const h = cross(direction, edge2);
  const determinant = dot(edge1, h);
  if (Math.abs(determinant) <= 1e-12) return false;
  const inverse = 1 / determinant;
  const s = subtract(start, triangle.vertices[0]);
  const u = inverse * dot(s, h);
  const q = cross(s, edge1);
  const v = inverse * dot(direction, q);
  const t = inverse * dot(edge2, q);
  return u > BARYCENTRIC_EPSILON && v > BARYCENTRIC_EPSILON && u + v < 1 - BARYCENTRIC_EPSILON && t > BARYCENTRIC_EPSILON && t < 1 - BARYCENTRIC_EPSILON;
}

/** Whether two corners at the same 2-D origin overlap with some area (touching along a ray does not count). */
function cornersOverlap(a1: readonly [number, number], a2: readonly [number, number], b1: readonly [number, number], b2: readonly [number, number]): boolean {
  const turn = (p: readonly [number, number], q: readonly [number, number]): number => Math.atan2(p[0] * q[1] - p[1] * q[0], p[0] * q[0] + p[1] * q[1]);
  const spanA = turn(a1, a2); const from = spanA >= 0 ? a1 : a2; const widthA = Math.abs(spanA);
  const spanB = turn(b1, b2); const widthB = Math.abs(spanB);
  /* angles of corner B measured from the first ray of corner A: the corners are [0, widthA] and [start, start + widthB] on the circle */
  const start = turn(from, spanB >= 0 ? b1 : b2);
  const ANGLE_EPSILON = 1e-4;
  const overlap = (offset: number): number => Math.min(widthA, offset + widthB) - Math.max(0, offset);
  return Math.max(overlap(start), overlap(start - 2 * Math.PI), overlap(start + 2 * Math.PI)) > ANGLE_EPSILON;
}

/**
 * The common case between neighbours, decided without allocating: the corners of `other` that are not shared (those not in the `matched` mask) lie off the
 * plane of `triangle`, all on one side of it, so the two meet only in what they share.
 */
function separatedByPlane(triangle: MeshIntersectionTriangle, other: MeshIntersectionTriangle, matched: number, normals: Float64Array, index: number): boolean {
  const at = index * 4;
  const origin = triangle.vertices[0];
  if (normals[at + 3] === 0) {
    const b = triangle.vertices[1]; const c = triangle.vertices[2];
    const ux = b[0] - origin[0]; const uy = b[1] - origin[1]; const uz = b[2] - origin[2];
    const vx = c[0] - origin[0]; const vy = c[1] - origin[1]; const vz = c[2] - origin[2];
    const nx = uy * vz - uz * vy; const ny = uz * vx - ux * vz; const nz = ux * vy - uy * vx;
    const length = Math.sqrt(nx * nx + ny * ny + nz * nz);
    if (length <= 1e-12) return false;
    normals[at] = nx / length; normals[at + 1] = ny / length; normals[at + 2] = nz / length; normals[at + 3] = 1;
  }
  let side = 0;
  for (let corner = 0; corner < 3; corner += 1) {
    if ((matched & (1 << corner)) !== 0) continue;
    const p = other.vertices[corner];
    const distance = (p[0] - origin[0]) * normals[at] + (p[1] - origin[1]) * normals[at + 1] + (p[2] - origin[2]) * normals[at + 2];
    if (Math.abs(distance) <= 1e-6) return false;                     /* on the plane: the full test decides */
    const sign = distance > 0 ? 1 : -1;
    if (side !== 0 && sign !== side) return false;
    side = sign;
  }
  return side !== 0;
}

/**
 * Two triangles that share vertices touch by design; they intersect only if they ALSO overlap away from what they share: a duplicate face, a face folded back
 * flat onto its edge neighbour, or, with one shared vertex, an opposite edge piercing the other face or two flat corners covering each other.
 */
function neighboursIntersect(a: MeshIntersectionTriangle, b: MeshIntersectionTriangle, sharedInA: number[], epsilonMeters: number): boolean {
  const normalA = cross(subtract(a.vertices[1], a.vertices[0]), subtract(a.vertices[2], a.vertices[0]));
  const normalB = cross(subtract(b.vertices[1], b.vertices[0]), subtract(b.vertices[2], b.vertices[0]));
  const lengthA = Math.hypot(...normalA); const lengthB = Math.hypot(...normalB);
  if (lengthA <= 1e-12 || lengthB <= 1e-12) return false;
  if (sharedInA.length >= 3) return true;
  const sine = Math.hypot(...cross(normalA, normalB)) / (lengthA * lengthB);
  const ownA = [0, 1, 2].filter((i) => !sharedInA.includes(i)).map((i) => a.vertices[i]);
  const ownB = [0, 1, 2].filter((i) => !sharedInA.some((j) => a.keys[j] === b.keys[i])).map((i) => b.vertices[i]);
  const offPlane = Math.max(...ownB.map((point) => Math.abs(dot(subtract(point, a.vertices[0]), normalA)) / lengthA));
  const coplanar = sine <= 1e-5 || offPlane <= Math.min(epsilonMeters, 1e-6);
  const corner = a.vertices[sharedInA[0]];
  if (sharedInA.length === 2) {
    if (!coplanar) return false;                                   /* two planes through the same edge meet only along it */
    const edge = subtract(a.vertices[sharedInA[1]], corner);
    return dot(cross(edge, subtract(ownA[0], corner)), cross(edge, subtract(ownB[0], corner))) > 0;
  }
  if (!coplanar) return segmentPiercesTriangle(ownA[0], ownA[1], b) || segmentPiercesTriangle(ownB[0], ownB[1], a);
  const axis = dominantAxis(normalA);
  const ray = (point: MeshIntersectionPoint): readonly [number, number] => { const p = project2(point, axis); const o = project2(corner, axis); return [p[0] - o[0], p[1] - o[1]]; };
  return cornersOverlap(ray(ownA[0]), ray(ownA[1]), ray(ownB[0]), ray(ownB[1]));
}

function grid(value: number, cellSizeMeters: number): number {
  return Math.floor(value / cellSizeMeters);
}

/** One number per grid cell: 17 bits per axis around the origin (hundreds of kilometres at the smallest cell). */
function cellKey(x: number, y: number, z: number): number {
  return ((x + 65536) * 131072 + (y + 65536)) * 131072 + (z + 65536);
}

function stableTrianglePriority(
  triangle: MeshIntersectionTriangle,
  aabb: Aabb,
  cellSizeMeters: number,
): number {
  const quantization = Math.max(cellSizeMeters / 16, 1e-5);
  const vertices = triangle.vertices
    .map((point) => [
      Math.round(point[0] / quantization),
      Math.round(point[1] / quantization),
      Math.round(point[2] / quantization),
    ] as const)
    .sort((left, right) => left[0] - right[0] || left[1] - right[1] || left[2] - right[2]);
  const cx = Math.round(((aabb.minX + aabb.maxX) * 0.5) / cellSizeMeters);
  const cy = Math.round(((aabb.minY + aabb.maxY) * 0.5) / cellSizeMeters);
  const cz = Math.round(((aabb.minZ + aabb.maxZ) * 0.5) / cellSizeMeters);
  let hash = 0x811c9dc5;
  for (const value of [cx, cy, cz, ...vertices.flat()]) hash = Math.imul(hash ^ value, 0x01000193);
  hash ^= hash >>> 16;
  return hash >>> 0;
}

/**
 * Self-intersection diagnostic for mobile-generated triangle soup. A grid finds every pair whose boxes overlap; faces that share quantized vertices
 * are judged as neighbours (a legitimate sharp fold is not an intersection). Positive findings stay useful when truncated; a truncated zero count must never
 * be read as proof of a clean mesh.
 */
export function diagnoseMeshSelfIntersections(
  triangles: readonly MeshIntersectionTriangle[],
  componentByTriangle: Int32Array,
  componentCount: number,
  voxelSizeMeters: number,
): MeshSelfIntersectionDiagnostic {
  const triangleCount = triangles.length;
  const triangleIntersectionHits = new Uint16Array(triangleCount);
  const componentPairCounts = new Uint32Array(Math.max(0, componentCount));
  const aabbs = triangles.map(aabbForTriangle);
  const cellSizeMeters = clamp(Math.abs(voxelSizeMeters) * 2.5, 0.065, 0.24);
  const epsilonMeters = clamp(Math.abs(voxelSizeMeters) * 0.015, 1e-5, 8e-4);
  /* The order in which pairs are tested is fixed by the geometry, not by the order of the triangles, so a truncated result does not depend on it. */
  const priorities = triangles.map((triangle, index) => stableTrianglePriority(triangle, aabbs[index], cellSizeMeters));
  const order = Array.from({ length: triangleCount }, (_, index) => index).sort((left, right) =>
    priorities[left] - priorities[right] || left - right
  );
  const rank = new Int32Array(triangleCount);
  order.forEach((triangleIndex, position) => { rank[triangleIndex] = position; });
  const budgets = selfIntersectionBudgets(triangleCount);
  let broadPhaseCandidatePairs = 0;
  let exactTestedPairs = 0;
  let intersectingPairCount = 0;
  const oversizedTriangleCount = 0;
  let truncated = false;

  const positions = new Float32Array(triangleCount * 9);
  triangles.forEach((triangle, index) => triangle.vertices.forEach((vertex, corner) => positions.set(vertex, index * 9 + corner * 3)));
  const normals = new Float64Array(triangleCount * 4);                /* unit normal and 1 once computed */
  const margin = epsilonMeters;
  const buckets = new Map<number, number[]>();
  const large: number[] = [];                                          /* triangles too wide for the grid: every later one is compared with them */
  const inserted: number[] = [];
  const seenBy = new Int32Array(triangleCount).fill(-1);
  let largeComparisons = 0;
  const candidates: number[] = [];
  const consider = (otherIndex: number, current: number): void => {
    if (seenBy[otherIndex] === current) return;
    seenBy[otherIndex] = current;
    if (aabbOverlap(aabbs[current], aabbs[otherIndex], epsilonMeters)) candidates.push(otherIndex);
  };

  for (const triangleIndex of order) {
    if (truncated) break;
    const bounds = aabbs[triangleIndex];
    const minX = grid(bounds.minX - margin, cellSizeMeters); const maxX = grid(bounds.maxX + margin, cellSizeMeters);
    const minY = grid(bounds.minY - margin, cellSizeMeters); const maxY = grid(bounds.maxY + margin, cellSizeMeters);
    const minZ = grid(bounds.minZ - margin, cellSizeMeters); const maxZ = grid(bounds.maxZ + margin, cellSizeMeters);
    candidates.length = 0;
    if ((maxX - minX + 1) * (maxY - minY + 1) * (maxZ - minZ + 1) > MAX_SELF_INTERSECTION_CELLS_PER_TRIANGLE) {
      largeComparisons += inserted.length;
      if (largeComparisons > MAX_SELF_INTERSECTION_LARGE_COMPARISONS) truncated = true;
      for (const otherIndex of inserted) consider(otherIndex, triangleIndex);
      large.push(triangleIndex);
    } else {
      for (const otherIndex of large) consider(otherIndex, triangleIndex);
      for (let x = minX; x <= maxX; x += 1) for (let y = minY; y <= maxY; y += 1) for (let z = minZ; z <= maxZ; z += 1) {
        const key = cellKey(x, y, z);
        const bucket = buckets.get(key);
        if (bucket === undefined) { buckets.set(key, [triangleIndex]); continue; }
        for (const otherIndex of bucket) consider(otherIndex, triangleIndex);
        bucket.push(triangleIndex);
      }
    }
    inserted.push(triangleIndex);
    if (exactTestedPairs + candidates.length > budgets.exactTests) candidates.sort((left, right) => rank[left] - rank[right]);   /* the order matters only when the cap cuts */
    for (const otherIndex of candidates) {
      const triangle = triangles[triangleIndex];
      const other = triangles[otherIndex];
      broadPhaseCandidatePairs += 1;
      if (exactTestedPairs >= budgets.exactTests) { truncated = true; break; }
      exactTestedPairs += 1;
      /* which corners of this triangle the other one has too, and which corners of the other one those are (bit masks) */
      let sharedCount = 0; let matched = 0;
      for (let corner = 0; corner < 3; corner += 1) {
        const key = triangle.keys[corner];
        const at = key === other.keys[0] ? 0 : key === other.keys[1] ? 1 : key === other.keys[2] ? 2 : -1;
        if (at >= 0) { sharedCount += 1; matched |= 1 << at; }
      }
      if (sharedCount === 0) {
        if (!trianglesIntersect(triangle, other, epsilonMeters)) continue;
      } else {
        if (sharedCount < 3 && separatedByPlane(triangle, other, matched, normals, triangleIndex)) continue;
        const shared = [0, 1, 2].filter((corner) => other.keys.includes(triangle.keys[corner]));
        if (!neighboursIntersect(triangle, other, shared, epsilonMeters)) continue;
      }
      intersectingPairCount += 1;
      if (triangleIntersectionHits[triangleIndex] < 0xffff) triangleIntersectionHits[triangleIndex] += 1;
      if (triangleIntersectionHits[otherIndex] < 0xffff) triangleIntersectionHits[otherIndex] += 1;
      const firstComponent = componentByTriangle[triangleIndex] ?? -1;
      const secondComponent = componentByTriangle[otherIndex] ?? -1;
      if (firstComponent >= 0 && firstComponent < componentPairCounts.length) componentPairCounts[firstComponent] += 1;
      if (
        secondComponent >= 0 && secondComponent < componentPairCounts.length &&
        secondComponent !== firstComponent
      ) componentPairCounts[secondComponent] += 1;
    }
  }

  let affectedTriangleCount = 0;
  for (const hits of triangleIntersectionHits) if (hits > 0) affectedTriangleCount += 1;
  const affectedByComponent = new Uint32Array(Math.max(0, componentCount));
  const triangleCountByComponent = new Uint32Array(Math.max(0, componentCount));
  for (let triangle = 0; triangle < triangleCount; triangle += 1) {
    const component = componentByTriangle[triangle] ?? -1;
    if (component < 0 || component >= componentCount) continue;
    triangleCountByComponent[component] += 1;
    if (triangleIntersectionHits[triangle] > 0) affectedByComponent[component] += 1;
  }
  const componentStats: MeshSelfIntersectionComponentStats[] = Array.from(
    { length: Math.max(0, componentCount) },
    (_, component) => ({
      intersectingPairCount: componentPairCounts[component],
      affectedTriangleCount: affectedByComponent[component],
      affectedTriangleRatio: triangleCountByComponent[component] > 0
        ? affectedByComponent[component] / triangleCountByComponent[component]
        : 0,
    }),
  );

  return {
    broadPhaseCandidatePairs,
    exactTestedPairs,
    intersectingPairCount,
    affectedTriangleCount,
    truncated,
    oversizedTriangleCount,
    triangleIntersectionHits,
    componentStats,
  };
}
