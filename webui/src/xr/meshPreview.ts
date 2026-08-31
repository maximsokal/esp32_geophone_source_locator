import {
  clamp01,
  crossVector,
  dotVector,
  normalizeVector,
  normalsCompatible,
  type ProjectVector,
} from './surfelEvidence';

export interface MeshPreviewInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
}

export interface MeshPreview {
  /** Expanded XYZ triangles in project coordinates for WebGL drawArrays. */
  trianglePositions: Float32Array;
  triangleCount: number;
  sourcePointCount: number;
  acceptedPointCount: number;
  rejectedTriangleCount: number;
  searchRadiusMeters: number;
}

interface PointRecord {
  index: number;
  position: readonly [number, number, number];
  normal: ProjectVector;
  confidence: number;
}

interface Neighbour {
  point: PointRecord;
  distance: number;
  angle: number;
}

const MESH_MIN_CONFIDENCE = 0.58;
const MESH_MAX_SURFACE_ANGLE_RADIANS = (45 * Math.PI) / 180;
const MESH_MIN_TRIANGLE_ANGLE_RADIANS = (10 * Math.PI) / 180;
const MESH_MAX_TRIANGLE_ANGLE_RADIANS = (120 * Math.PI) / 180;
const MESH_MAX_NEIGHBOURS = 12;
const MESH_MAX_TRIANGLES = 30000;
const MESH_MAX_POINTS = 12000;

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function grid(value: number, size: number): number {
  return Math.floor(value / size);
}

function key(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function subtract(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): ProjectVector {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function distance(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function tangentBasis(normal: ProjectVector): readonly [ProjectVector, ProjectVector] {
  const n = normalizeVector(normal);
  const helper: ProjectVector = Math.abs(n[2]) < 0.82 ? [0, 0, 1] : [0, 1, 0];
  const u = crossVector(helper, n);
  const v = crossVector(n, u);
  return [u, v];
}

function angleAt(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
  c: readonly [number, number, number],
): number {
  const ba = normalizeVector(subtract(a, b));
  const bc = normalizeVector(subtract(c, b));
  return Math.acos(clamp(dotVector(ba, bc), -1, 1));
}

export function triangleGeometryAcceptable(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
  c: readonly [number, number, number],
  maxEdgeMeters: number,
): boolean {
  if (!Number.isFinite(maxEdgeMeters) || maxEdgeMeters <= 0) return false;
  const ab = distance(a, b);
  const bc = distance(b, c);
  const ca = distance(c, a);
  if (Math.min(ab, bc, ca) < 1e-5 || Math.max(ab, bc, ca) > maxEdgeMeters) return false;

  const angles = [angleAt(b, a, c), angleAt(a, b, c), angleAt(a, c, b)];
  return angles.every((angle) =>
    Number.isFinite(angle) &&
    angle >= MESH_MIN_TRIANGLE_ANGLE_RADIANS &&
    angle <= MESH_MAX_TRIANGLE_ANGLE_RADIANS,
  );
}

function triangleNormal(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
  c: readonly [number, number, number],
): ProjectVector {
  return crossVector(subtract(b, a), subtract(c, a));
}

function recordsFromInput(input: MeshPreviewInput): PointRecord[] {
  const count = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
    MESH_MAX_POINTS,
  );
  const records: PointRecord[] = [];
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    const confidence = clamp01(input.confidence[index]);
    if (confidence < MESH_MIN_CONFIDENCE) continue;
    const position = [
      input.positions[offset], input.positions[offset + 1], input.positions[offset + 2],
    ] as const;
    const normal = normalizeVector([
      input.normals[offset], input.normals[offset + 1], input.normals[offset + 2],
    ]);
    if (![...position, ...normal].every(Number.isFinite)) continue;
    records.push({ index, position, normal, confidence });
  }
  return records;
}

/**
 * Conservative GP3-style local triangulation for the phone preview.
 *
 * This is intentionally not a watertight Poisson reconstruction. It follows
 * the same safety principles as PCL GreedyProjectionTriangulation: spatial
 * search radius, tangent-plane neighbour ordering, triangle-angle limits and a
 * maximum normal/surface angle so opposite sides of a sharp edge are not
 * connected merely because they are spatially close.
 */
export function buildSurfaceMeshPreview(input: MeshPreviewInput): MeshPreview {
  const sourcePointCount = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
  );
  const points = recordsFromInput(input);
  const searchRadiusMeters = clamp(input.voxelSizeMeters * 3.2, 0.065, 0.18);
  if (points.length < 3) {
    return {
      trianglePositions: new Float32Array(0), triangleCount: 0,
      sourcePointCount, acceptedPointCount: points.length,
      rejectedTriangleCount: 0, searchRadiusMeters,
    };
  }

  const buckets = new Map<string, PointRecord[]>();
  for (const point of points) {
    const [x, y, z] = point.position;
    const bucketKey = key(grid(x, searchRadiusMeters), grid(y, searchRadiusMeters), grid(z, searchRadiusMeters));
    const bucket = buckets.get(bucketKey) ?? [];
    bucket.push(point);
    buckets.set(bucketKey, bucket);
  }

  const triangles: number[] = [];
  const emitted = new Set<string>();
  let rejectedTriangleCount = 0;

  for (const center of points) {
    if (triangles.length / 9 >= MESH_MAX_TRIANGLES) break;
    const [cx, cy, cz] = center.position;
    const gx = grid(cx, searchRadiusMeters);
    const gy = grid(cy, searchRadiusMeters);
    const gz = grid(cz, searchRadiusMeters);
    const [basisU, basisV] = tangentBasis(center.normal);
    const neighbours: Neighbour[] = [];

    for (let dx = -1; dx <= 1; dx += 1) {
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dz = -1; dz <= 1; dz += 1) {
          const bucket = buckets.get(key(gx + dx, gy + dy, gz + dz));
          if (bucket === undefined) continue;
          for (const candidate of bucket) {
            if (candidate.index === center.index) continue;
            const d = distance(center.position, candidate.position);
            if (d <= 1e-5 || d > searchRadiusMeters) continue;
            if (!normalsCompatible(center.normal, candidate.normal, MESH_MAX_SURFACE_ANGLE_RADIANS)) continue;
            const delta = subtract(candidate.position, center.position);
            neighbours.push({
              point: candidate,
              distance: d,
              angle: Math.atan2(dotVector(delta, basisV), dotVector(delta, basisU)),
            });
          }
        }
      }
    }

    neighbours.sort((left, right) => left.distance - right.distance);
    const selected = neighbours.slice(0, MESH_MAX_NEIGHBOURS);
    selected.sort((left, right) => left.angle - right.angle);
    if (selected.length < 2) continue;

    for (let index = 0; index < selected.length; index += 1) {
      const first = selected[index].point;
      const second = selected[(index + 1) % selected.length].point;
      if (first.index === second.index) continue;
      const ids = [center.index, first.index, second.index].sort((a, b) => a - b);
      const triangleKey = `${ids[0]}:${ids[1]}:${ids[2]}`;
      if (emitted.has(triangleKey)) continue;
      emitted.add(triangleKey);

      if (!triangleGeometryAcceptable(center.position, first.position, second.position, searchRadiusMeters)) {
        rejectedTriangleCount += 1;
        continue;
      }
      const triNormal = triangleNormal(center.position, first.position, second.position);
      if (
        !normalsCompatible(triNormal, center.normal, MESH_MAX_SURFACE_ANGLE_RADIANS) ||
        !normalsCompatible(triNormal, first.normal, MESH_MAX_SURFACE_ANGLE_RADIANS) ||
        !normalsCompatible(triNormal, second.normal, MESH_MAX_SURFACE_ANGLE_RADIANS)
      ) {
        rejectedTriangleCount += 1;
        continue;
      }

      triangles.push(
        ...center.position,
        ...first.position,
        ...second.position,
      );
      if (triangles.length / 9 >= MESH_MAX_TRIANGLES) break;
    }
  }

  return {
    trianglePositions: Float32Array.from(triangles),
    triangleCount: triangles.length / 9,
    sourcePointCount,
    acceptedPointCount: points.length,
    rejectedTriangleCount,
    searchRadiusMeters,
  };
}
