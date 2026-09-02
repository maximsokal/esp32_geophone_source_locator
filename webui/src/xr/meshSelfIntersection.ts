export type MeshIntersectionPoint = readonly [number, number, number];

export interface MeshIntersectionTriangle {
  vertices: readonly [MeshIntersectionPoint, MeshIntersectionPoint, MeshIntersectionPoint];
  keys: readonly [string, string, string];
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

export const MAX_SELF_INTERSECTION_BROAD_PHASE_PAIRS = 60_000;
export const MAX_SELF_INTERSECTION_EXACT_TESTS = 12_000;
export const MAX_SELF_INTERSECTION_CELLS_PER_TRIANGLE = 64;
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

function sharedVertex(a: MeshIntersectionTriangle, b: MeshIntersectionTriangle): boolean {
  return a.keys[0] === b.keys[0] || a.keys[0] === b.keys[1] || a.keys[0] === b.keys[2] ||
    a.keys[1] === b.keys[0] || a.keys[1] === b.keys[1] || a.keys[1] === b.keys[2] ||
    a.keys[2] === b.keys[0] || a.keys[2] === b.keys[1] || a.keys[2] === b.keys[2];
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

function grid(value: number, cellSizeMeters: number): number {
  return Math.floor(value / cellSizeMeters);
}

function cellKey(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function pairKey(a: number, b: number, triangleCount: number): number {
  const low = Math.min(a, b);
  const high = Math.max(a, b);
  return low * triangleCount + high;
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
 * Bounded self-intersection diagnostic for mobile-generated triangle soup.
 *
 * A spatial AABB hash eliminates the O(T^2) all-pairs scan. Faces sharing any
 * quantized vertex are deliberately skipped: a legitimate sharp 90-degree fold
 * is adjacent geometry, not self-intersection noise. The exact triangle tests are
 * globally capped, so even a pathological 50k-face soup cannot monopolize the
 * worker indefinitely. Positive intersections are still useful when truncated;
 * a truncated zero count must never be interpreted as proof of a clean mesh.
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
  const priorities = triangles.map((triangle, index) => stableTrianglePriority(triangle, aabbs[index], cellSizeMeters));
  const buckets = new Map<string, number[]>();
  const seenPairs = new Set<number>();
  const order = Array.from({ length: triangleCount }, (_, index) => index).sort((left, right) =>
    priorities[left] - priorities[right] || left - right
  );
  let broadPhaseCandidatePairs = 0;
  let exactTestedPairs = 0;
  let intersectingPairCount = 0;
  let oversizedTriangleCount = 0;
  let truncated = false;

  outer: for (const triangleIndex of order) {
    const bounds = aabbs[triangleIndex];
    const minX = grid(bounds.minX, cellSizeMeters);
    const minY = grid(bounds.minY, cellSizeMeters);
    const minZ = grid(bounds.minZ, cellSizeMeters);
    const maxX = grid(bounds.maxX, cellSizeMeters);
    const maxY = grid(bounds.maxY, cellSizeMeters);
    const maxZ = grid(bounds.maxZ, cellSizeMeters);
    const cellCount = (maxX - minX + 1) * (maxY - minY + 1) * (maxZ - minZ + 1);
    if (cellCount > MAX_SELF_INTERSECTION_CELLS_PER_TRIANGLE) {
      oversizedTriangleCount += 1;
      truncated = true;
      continue;
    }

    for (let x = minX; x <= maxX; x += 1) {
      for (let y = minY; y <= maxY; y += 1) {
        for (let z = minZ; z <= maxZ; z += 1) {
          const key = cellKey(x, y, z);
          const bucket = buckets.get(key);
          if (bucket !== undefined) {
            for (const otherIndex of bucket) {
              const triangle = triangles[triangleIndex];
              const other = triangles[otherIndex];
              // Adjacent manifold faces and cell-coincident but non-overlapping
              // AABBs are cheap rejects and must not consume the bounded pair budget.
              if (sharedVertex(triangle, other) || !aabbOverlap(bounds, aabbs[otherIndex], epsilonMeters)) continue;
              const encoded = pairKey(triangleIndex, otherIndex, triangleCount);
              if (seenPairs.has(encoded)) continue;
              if (broadPhaseCandidatePairs >= MAX_SELF_INTERSECTION_BROAD_PHASE_PAIRS) {
                truncated = true;
                break outer;
              }
              seenPairs.add(encoded);
              broadPhaseCandidatePairs += 1;
              if (exactTestedPairs >= MAX_SELF_INTERSECTION_EXACT_TESTS) {
                truncated = true;
                break outer;
              }
              exactTestedPairs += 1;
              if (!trianglesIntersect(triangle, other, epsilonMeters)) continue;
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
          if (bucket === undefined) buckets.set(key, [triangleIndex]);
          else bucket.push(triangleIndex);
        }
      }
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
