import {
  clamp01,
  crossVector,
  dotVector,
  normalizeVector,
  normalsCompatible,
  type ProjectVector,
} from './surfelEvidence';
import { analyzeSurfaceComponents } from './surfaceComponents';
import { sanitizeMeshTopology } from './meshTopologySanity';

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
  /** Triangles produced before topology sanitation. */
  rawTriangleCount: number;
  sourcePointCount: number;
  /** Points that passed confidence/readiness and belong to mesh-sized components. */
  acceptedPointCount: number;
  /** Confidence-valid points before physical component-size gating. */
  certifiedPointCount: number;
  rejectedTriangleCount: number;
  quarantinedTriangleCount: number;
  topologyDiscardedTriangleCount: number;
  meshTopologyComponentCount: number;
  nonManifoldEdgeCount: number;
  orientationRepairCount: number;
  orientationConflictCount: number;
  /** Absolute maximum triangulation radius; each point uses a smaller density-adaptive radius when possible. */
  searchRadiusMeters: number;
  componentCount: number;
  eligibleComponentCount: number;
  meshReadiness: number;
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

type DistanceEvidence = number | { readonly distance: number };

const MESH_MIN_CONFIDENCE = 0.58;
const MESH_MAX_SURFACE_ANGLE_RADIANS = (45 * Math.PI) / 180;
const MESH_MIN_TRIANGLE_ANGLE_RADIANS = (10 * Math.PI) / 180;
const MESH_MAX_TRIANGLE_ANGLE_RADIANS = (120 * Math.PI) / 180;
const MESH_MAX_NEIGHBOURS = 12;
const MESH_MAX_TRIANGLES = 30000;
const MESH_MAX_POINTS = 12000;
const MESH_DENSITY_MU = 2.8;

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function grid(value: number, size: number): number {
  return Math.floor(value / size);
}

function key(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function meshEdgeKey(a: number, b: number): string {
  return a < b ? `${a}:${b}` : `${b}:${a}`;
}

function compareMeshEdges(a0: number, a1: number, b0: number, b1: number): number {
  const amin = Math.min(a0, a1), amax = Math.max(a0, a1);
  const bmin = Math.min(b0, b1), bmax = Math.max(b0, b1);
  return amin - bmin || amax - bmax;
}

function gabrielEdgeAllowed(
  first: PointRecord,
  second: PointRecord,
  localPool: readonly PointRecord[],
): boolean {
  const dx = second.position[0] - first.position[0];
  const dy = second.position[1] - first.position[1];
  const dz = second.position[2] - first.position[2];
  const edgeLengthSquared = dx * dx + dy * dy + dz * dz;
  if (!Number.isFinite(edgeLengthSquared) || edgeLengthSquared <= 1e-10) return false;
  const mx = (first.position[0] + second.position[0]) * 0.5;
  const my = (first.position[1] + second.position[1]) * 0.5;
  const mz = (first.position[2] + second.position[2]) * 0.5;
  const radiusSquared = edgeLengthSquared * 0.25;
  const tolerance = Math.max(1e-10, radiusSquared * 1e-5);
  const boundary: number[] = [];
  for (const candidate of localPool) {
    if (candidate.index === first.index || candidate.index === second.index) continue;
    const px = candidate.position[0] - mx;
    const py = candidate.position[1] - my;
    const pz = candidate.position[2] - mz;
    const squared = px * px + py * py + pz * pz;
    if (squared < radiusSquared - tolerance) return false;
    if (Math.abs(squared - radiusSquared) <= tolerance) boundary.push(candidate.index);
  }
  if (boundary.length >= 2) {
    boundary.sort((left, right) => left - right);
    // Four co-circular samples admit two Delaunay diagonals. Choose one globally
    // by source-index edge order so adjacent center fans cannot choose both.
    if (compareMeshEdges(first.index, second.index, boundary[0], boundary[1]) > 0) return false;
  }
  return true;
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

/**
 * GP3-style density multiplier: the global search radius remains a hard maximum,
 * but dense local surfels should create correspondingly smaller triangles.
 *
 * We use the third compatible neighbour rather than the absolute nearest point.
 * The accumulator may retain multiple close surfels around sharp features, and a
 * single near-duplicate must not collapse the triangulation radius to millimetres.
 * The three-nearest selection is allocation-free so this helper can run once per
 * mesh center without adding mobile GC pressure.
 */
export function adaptiveMeshSearchRadius(
  neighbourDistances: readonly DistanceEvidence[],
  maximumRadiusMeters: number,
  voxelSizeMeters: number,
): number {
  const maximum = Math.max(1e-4, Number.isFinite(maximumRadiusMeters) ? maximumRadiusMeters : 0.1);
  let first = Number.POSITIVE_INFINITY;
  let second = Number.POSITIVE_INFINITY;
  let third = Number.POSITIVE_INFINITY;
  let count = 0;
  for (const item of neighbourDistances) {
    const value = typeof item === 'number' ? item : item.distance;
    if (!Number.isFinite(value) || value <= 1e-5 || value > maximum) continue;
    count += 1;
    if (value < first) {
      third = second; second = first; first = value;
    } else if (value < second) {
      third = second; second = value;
    } else if (value < third) {
      third = value;
    }
  }
  if (count < 2) return maximum;
  const reference = Number.isFinite(third) ? third : second;
  const minimum = Math.min(maximum, Math.max(0.025, Math.abs(voxelSizeMeters) * 0.70));
  return clamp(reference * MESH_DENSITY_MU, minimum, maximum);
}

function triangleNormal(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
  c: readonly [number, number, number],
): ProjectVector {
  return crossVector(subtract(b, a), subtract(c, a));
}

function recordsFromInput(input: MeshPreviewInput, eligiblePointMask: Uint8Array): PointRecord[] {
  const count = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
    eligiblePointMask.length,
    MESH_MAX_POINTS,
  );
  const records: PointRecord[] = [];
  for (let index = 0; index < count; index += 1) {
    if (eligiblePointMask[index] === 0) continue;
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

function emptyTopologyDiagnostics() {
  return {
    rawTriangleCount: 0,
    quarantinedTriangleCount: 0,
    topologyDiscardedTriangleCount: 0,
    meshTopologyComponentCount: 0,
    nonManifoldEdgeCount: 0,
    orientationRepairCount: 0,
    orientationConflictCount: 0,
  };
}

/**
 * Conservative GP3-style local triangulation for the phone preview.
 *
 * Before triangulation, a bounded voxel-hash component pass rejects dense but
 * physically tiny islands and one-dimensional point chains. Only components
 * with enough measured 2-D extent, occupied cells and support density can enter
 * the local triangulator.
 *
 * The triangulation follows PCL GP3 safety principles: an absolute search radius,
 * a local mu-style radius derived from measured neighbour spacing, tangent-plane
 * neighbour ordering, triangle-angle limits and a maximum normal/surface angle.
 * More depth samples on a convex feature therefore increase local detail instead
 * of permitting large triangles across the denser measurements.
 *
 * A final topology pass never edits physical surfels/occupancy. It only removes
 * tiny or topologically suspect triangle islands from the mesh representation,
 * repairs local winding and reports non-manifold/orientation diagnostics.
 */
export function buildSurfaceMeshPreview(input: MeshPreviewInput): MeshPreview {
  const sourcePointCount = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
  );
  const componentAnalysis = analyzeSurfaceComponents({
    positions: input.positions,
    normals: input.normals,
    confidence: input.confidence,
    voxelSizeMeters: input.voxelSizeMeters,
    minimumConfidence: MESH_MIN_CONFIDENCE,
    maxPoints: MESH_MAX_POINTS,
  });
  const points = recordsFromInput(input, componentAnalysis.eligiblePointMask);
  const searchRadiusMeters = clamp(input.voxelSizeMeters * 3.2, 0.065, 0.18);
  const baseResult = {
    sourcePointCount,
    acceptedPointCount: points.length,
    certifiedPointCount: componentAnalysis.acceptedPointCount,
    componentCount: componentAnalysis.componentCount,
    eligibleComponentCount: componentAnalysis.eligibleComponentCount,
    meshReadiness: componentAnalysis.bestMeshComponent?.meshReadiness ?? 0,
    searchRadiusMeters,
  };
  if (points.length < 3) {
    return {
      ...baseResult,
      ...emptyTopologyDiagnostics(),
      trianglePositions: new Float32Array(0), triangleCount: 0,
      rejectedTriangleCount: 0,
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
  const emittedEdgeUseCount = new Map<string, number>();
  let geometryRejectedTriangleCount = 0;
  let manifoldRejectedTriangleCount = 0;

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

    neighbours.sort((left, right) => left.distance - right.distance || left.point.index - right.point.index);
    const localRadiusMeters = adaptiveMeshSearchRadius(
      neighbours,
      searchRadiusMeters,
      input.voxelSizeMeters,
    );
    const localNeighbours = neighbours
      .filter((neighbour) => neighbour.distance <= localRadiusMeters);
    // Bounded pool for the empty-diameter edge test. Twice the fan budget is
    // enough to see closer blockers while keeping mobile work deterministic.
    const gabrielPool = [center, ...localNeighbours.slice(0, MESH_MAX_NEIGHBOURS * 2).map((item) => item.point)];
    const selected = localNeighbours
      .slice(0, MESH_MAX_NEIGHBOURS)
      .filter((neighbour) => gabrielEdgeAllowed(center, neighbour.point, gabrielPool));
    selected.sort((left, right) => left.angle - right.angle || left.point.index - right.point.index);
    if (selected.length < 2) continue;

    for (let index = 0; index < selected.length; index += 1) {
      const first = selected[index].point;
      const second = selected[(index + 1) % selected.length].point;
      if (first.index === second.index) continue;
      const ids = [center.index, first.index, second.index].sort((a, b) => a - b);
      const triangleKey = `${ids[0]}:${ids[1]}:${ids[2]}`;
      if (emitted.has(triangleKey)) continue;
      emitted.add(triangleKey);

      if (!triangleGeometryAcceptable(center.position, first.position, second.position, localRadiusMeters)) {
        geometryRejectedTriangleCount += 1;
        continue;
      }
      if (!gabrielEdgeAllowed(first, second, gabrielPool)) {
        geometryRejectedTriangleCount += 1;
        continue;
      }
      const triNormal = triangleNormal(center.position, first.position, second.position);
      if (
        !normalsCompatible(triNormal, center.normal, MESH_MAX_SURFACE_ANGLE_RADIANS) ||
        !normalsCompatible(triNormal, first.normal, MESH_MAX_SURFACE_ANGLE_RADIANS) ||
        !normalsCompatible(triNormal, second.normal, MESH_MAX_SURFACE_ANGLE_RADIANS)
      ) {
        geometryRejectedTriangleCount += 1;
        continue;
      }

      const edgeKeys = [
        meshEdgeKey(center.index, first.index),
        meshEdgeKey(first.index, second.index),
        meshEdgeKey(second.index, center.index),
      ] as const;
      if (edgeKeys.some((edgeKey) => (emittedEdgeUseCount.get(edgeKey) ?? 0) >= 2)) {
        manifoldRejectedTriangleCount += 1;
        continue;
      }

      // normalsCompatible is intentionally sign-invariant because fused surfel
      // normals may arrive with either sign. For actual mesh winding we still
      // choose the sign that agrees with the measured center surfel. The topology
      // pass below then propagates consistent winding across shared manifold edges.
      if (dotVector(triNormal, center.normal) < 0) {
        triangles.push(...center.position, ...second.position, ...first.position);
      } else {
        triangles.push(...center.position, ...first.position, ...second.position);
      }
      for (const edgeKey of edgeKeys) {
        emittedEdgeUseCount.set(edgeKey, (emittedEdgeUseCount.get(edgeKey) ?? 0) + 1);
      }
      if (triangles.length / 9 >= MESH_MAX_TRIANGLES) break;
    }
  }

  const topology = sanitizeMeshTopology(Float32Array.from(triangles), input.voxelSizeMeters);
  return {
    ...baseResult,
    trianglePositions: topology.trianglePositions,
    triangleCount: topology.triangleCount,
    rawTriangleCount: topology.sourceTriangleCount,
    rejectedTriangleCount:
      geometryRejectedTriangleCount +
      manifoldRejectedTriangleCount +
      topology.quarantinedTriangleCount +
      topology.discardedTriangleCount,
    quarantinedTriangleCount: topology.quarantinedTriangleCount,
    topologyDiscardedTriangleCount: topology.discardedTriangleCount,
    meshTopologyComponentCount: topology.components.length,
    nonManifoldEdgeCount: topology.nonManifoldEdgeCount,
    orientationRepairCount: topology.orientationRepairCount,
    orientationConflictCount: topology.orientationConflictCount,
  };
}