import { sanitizeMeshTopology } from './meshTopologySanity';
import { analyzeSurfaceComponents } from './surfaceComponents';

export type Point3 = readonly [number, number, number];
export type Vector3 = readonly [number, number, number];

export interface SparseTsdfInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
  maxInputPoints?: number;
  maxNodes?: number;
  maxTriangles?: number;
}

export interface SparseTsdfMesh {
  trianglePositions: Float32Array;
  triangleCount: number;
  rawTriangleCount: number;
  quarantinedTriangleCount: number;
  topologyDiscardedTriangleCount: number;
  nonManifoldEdgeCount: number;
  nonManifoldRepairTriangleCount: number;
  orientationRepairCount: number;
  orientationConflictCount: number;
  meshTopologyComponentCount: number;
  selfIntersectionPairCount: number;
  selfIntersectionAffectedTriangleCount: number;
  selfIntersectionExactTestedPairs: number;
  selfIntersectionBroadPhaseCandidatePairs: number;
  selfIntersectionTruncated: boolean;
  selfIntersectionOversizedTriangleCount: number;
  /** Small correlated local noise regions omitted from otherwise retained mesh components. */
  localNoiseHotspotCount: number;
  localNoiseHotspotTriangleCount: number;
  /** Maximum component-level topology/noise diagnostic; never changes physical occupancy. */
  meshNoiseScore: number;
  sourcePointCount: number;
  integratedPointCount: number;
  skippedLowConfidence: number;
  rejectedComponentPoints: number;
  componentCount: number;
  eligibleComponentCount: number;
  meshReadiness: number;
  /** How many times the voxel was made coarser because the triangle budget would otherwise have cut the surface (0 = never). */
  coarsenings?: number;
  nodeCount: number;
  voxelSizeMeters: number;
  truncationMeters: number;
  computeMs: number;
}

interface TsdfNode {
  x: number;
  y: number;
  z: number;
  weightedValue: number;
  weight: number;
}

interface Sample {
  position: Point3;
  normal: Vector3;
  confidence: number;
}

const MIN_SURFACE_CONFIDENCE = 0.58;
const DEFAULT_MAX_INPUT_POINTS = 12000;
const DEFAULT_MAX_NODES = 180000;
const DEFAULT_MAX_TRIANGLES = 50000;
const MIN_NODE_WEIGHT = 0.10;
const MIN_TRIANGLE_AREA_FACTOR = 2.0e-4;

const CUBE_OFFSETS: readonly Point3[] = [
  [0,0,0], [1,0,0], [1,1,0], [0,1,0],
  [0,0,1], [1,0,1], [1,1,1], [0,1,1],
];

// Six tetrahedra sharing cube diagonal 0 -> 6. This avoids a large marching-cubes
// lookup table while keeping extraction deterministic and local.
const TETRAHEDRA: readonly (readonly [number, number, number, number])[] = [
  [0,5,1,6], [0,1,2,6], [0,2,3,6],
  [0,3,7,6], [0,7,4,6], [0,4,5,6],
];

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number { return clamp(value, 0, 1); }
function key(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }
function grid(value: number, size: number): number { return Math.round(value / size); }

function normalize(vector: Vector3): Vector3 | null {
  const length = Math.hypot(vector[0], vector[1], vector[2]);
  if (!Number.isFinite(length) || length < 1e-9) return null;
  return [vector[0] / length, vector[1] / length, vector[2] / length];
}

function dot(a: Vector3, b: Vector3): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function subtract(a: Point3, b: Point3): Vector3 {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function cross(a: Vector3, b: Vector3): Vector3 {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function triangleArea(a: Point3, b: Point3, c: Point3): number {
  const areaVector = cross(subtract(b, a), subtract(c, a));
  return Math.hypot(areaVector[0], areaVector[1], areaVector[2]) * 0.5;
}

function buildFreeIndex(
  positions: Float32Array | undefined,
  voxelSizeMeters: number | undefined,
): { size: number; buckets: Map<string, Point3[]> } | null {
  if (
    positions === undefined || positions.length < 3 ||
    voxelSizeMeters === undefined || !Number.isFinite(voxelSizeMeters) || voxelSizeMeters <= 0
  ) return null;
  const size = Math.max(voxelSizeMeters, 0.04);
  const buckets = new Map<string, Point3[]>();
  for (let offset = 0; offset + 2 < positions.length; offset += 3) {
    const point: Point3 = [positions[offset], positions[offset + 1], positions[offset + 2]];
    if (!point.every(Number.isFinite)) continue;
    const gx = Math.floor(point[0] / size);
    const gy = Math.floor(point[1] / size);
    const gz = Math.floor(point[2] / size);
    const bucketKey = key(gx, gy, gz);
    const bucket = buckets.get(bucketKey) ?? [];
    bucket.push(point);
    buckets.set(bucketKey, bucket);
  }
  return buckets.size > 0 ? { size, buckets } : null;
}

function nearestFreeDirection(sample: Sample, index: ReturnType<typeof buildFreeIndex>): Vector3 | null {
  if (index === null) return null;
  const { size, buckets } = index;
  const gx = Math.floor(sample.position[0] / size);
  const gy = Math.floor(sample.position[1] / size);
  const gz = Math.floor(sample.position[2] / size);
  let nearest: Point3 | null = null;
  let nearestSquared = Number.POSITIVE_INFINITY;
  for (let dx = -2; dx <= 2; dx += 1) {
    for (let dy = -2; dy <= 2; dy += 1) {
      for (let dz = -2; dz <= 2; dz += 1) {
        const bucket = buckets.get(key(gx + dx, gy + dy, gz + dz));
        if (bucket === undefined) continue;
        for (const point of bucket) {
          const delta = subtract(point, sample.position);
          const squared = dot(delta, delta);
          if (squared < nearestSquared) { nearestSquared = squared; nearest = point; }
        }
      }
    }
  }
  if (nearest === null) return null;
  const direction = normalize(subtract(nearest, sample.position));
  // Ignore distant FREE cells: they do not certify which local side of this
  // surface patch is free air.
  return nearestSquared <= (size * 3.5) ** 2 ? direction : null;
}

function orientTowardVerifiedFree(sample: Sample, index: ReturnType<typeof buildFreeIndex>): Sample {
  const freeDirection = nearestFreeDirection(sample, index);
  if (freeDirection === null || dot(sample.normal, freeDirection) >= 0) return sample;
  return {
    ...sample,
    normal: [-sample.normal[0], -sample.normal[1], -sample.normal[2]],
  };
}

function sampleAt(input: SparseTsdfInput, index: number): Sample | null {
  const offset = index * 3;
  const position: Point3 = [input.positions[offset], input.positions[offset + 1], input.positions[offset + 2]];
  const normal = normalize([input.normals[offset], input.normals[offset + 1], input.normals[offset + 2]]);
  const confidence = clamp01(input.confidence[index]);
  if (normal === null || !position.every(Number.isFinite) || !Number.isFinite(confidence)) return null;
  return { position, normal, confidence };
}

function interpolateZero(a: Point3, av: number, b: Point3, bv: number): Point3 {
  const denominator = av - bv;
  const t = Math.abs(denominator) < 1e-9 ? 0.5 : clamp(av / denominator, 0, 1);
  return [
    a[0] + (b[0] - a[0]) * t,
    a[1] + (b[1] - a[1]) * t,
    a[2] + (b[2] - a[2]) * t,
  ];
}

function polygonizeTetra(
  positions: readonly [Point3, Point3, Point3, Point3],
  values: readonly [number, number, number, number],
): readonly (readonly [Point3, Point3, Point3])[] {
  const inside: number[] = [];
  const outside: number[] = [];
  for (let index = 0; index < 4; index += 1) (values[index] < 0 ? inside : outside).push(index);
  if (inside.length === 0 || inside.length === 4) return [];

  const crossing = (a: number, b: number): Point3 =>
    interpolateZero(positions[a], values[a], positions[b], values[b]);

  if (inside.length === 1 || inside.length === 3) {
    const loneInside = inside.length === 1;
    const lone = loneInside ? inside[0] : outside[0];
    const other = loneInside ? outside : inside;
    const p0 = crossing(lone, other[0]);
    const p1 = crossing(lone, other[1]);
    const p2 = crossing(lone, other[2]);
    return loneInside ? [[p0, p1, p2]] : [[p0, p2, p1]];
  }

  const [i0, i1] = inside;
  const [o0, o1] = outside;
  const a = crossing(i0, o0);
  const b = crossing(i0, o1);
  const c = crossing(i1, o0);
  const d = crossing(i1, o1);
  return [[a, b, c], [b, d, c]];
}

/**
 * Build a sparse confidence-weighted TSDF from oriented promoted surfels.
 *
 * A bounded voxel-hash component pass runs before field integration. Dense but
 * physically tiny islands and one-dimensional chains cannot create a mesh just
 * because the global scan contains many points. Only independently connected
 * surface components with enough two-dimensional metric extent are integrated.
 *
 * Curless/Levoy style signed-distance fusion is represented here as an implicit
 * field over sparse grid nodes. Each eligible surfel contributes only inside a
 * bounded truncation band and a bounded tangent footprint; therefore disconnected
 * or unsupported regions are never filled merely to make a watertight mesh.
 * Verified FREE voxels are used only to orient the positive side of local
 * surface normals. UNKNOWN volume is not converted to occupied/free evidence.
 *
 * The extracted triangle soup passes through the same conservative topology
 * sanitizer as the live preview. Topology cleanup never feeds back into physical
 * occupancy; it only decides which reconstructed faces are trustworthy enough to
 * expose as a mesh.
 */
/**
 * The surface is extracted without a cut. A budget that was reached used to stop the extraction mid-way through the nodes, which are stored in the order
 * the points arrived, so the surface ended as bands: on the 2026-11-22 scans the floor mesh covered one metre of a four metre room. When the budget
 * would be exceeded, the field is rebuilt with a coarser voxel (fewer, larger triangles over the whole surface) instead, up to a few times; only if even the
 * coarsest voxel does not fit is the old cut kept, as a last resort.
 */
export const SPARSE_TSDF_COARSEN_FACTOR = 1.3;
export const SPARSE_TSDF_MAX_COARSENINGS = 3;

export function buildSparseTsdfMesh(input: SparseTsdfInput): SparseTsdfMesh {
  let attempt = 0;
  let current = input;
  for (;;) {
    const built = buildSparseTsdfMeshOnce(current);
    if (!built.budgetReached || attempt >= SPARSE_TSDF_MAX_COARSENINGS) return { ...built.mesh, coarsenings: attempt };
    attempt += 1;
    current = { ...current, voxelSizeMeters: current.voxelSizeMeters * SPARSE_TSDF_COARSEN_FACTOR };
  }
}

function buildSparseTsdfMeshOnce(input: SparseTsdfInput): { mesh: SparseTsdfMesh; budgetReached: boolean } {
  const started = typeof performance !== 'undefined' ? performance.now() : Date.now();
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
    minimumConfidence: MIN_SURFACE_CONFIDENCE,
  });
  const voxelSizeMeters = clamp(
    Number.isFinite(input.voxelSizeMeters) ? input.voxelSizeMeters * 1.15 : 0.046,
    0.035,
    0.075,
  );
  const truncationMeters = voxelSizeMeters * 2.75;
  const tangentRadiusMeters = voxelSizeMeters * 2.25;
  const tangentSigmaMeters = voxelSizeMeters * 1.15;
  const maxInputPoints = Math.max(1, Math.floor(input.maxInputPoints ?? DEFAULT_MAX_INPUT_POINTS));
  const maxNodes = Math.max(1000, Math.floor(input.maxNodes ?? DEFAULT_MAX_NODES));
  const maxTriangles = Math.max(1, Math.floor(input.maxTriangles ?? DEFAULT_MAX_TRIANGLES));
  const freeIndex = buildFreeIndex(input.freeSpacePositions, input.freeSpaceVoxelSizeMeters);
  const nodes = new Map<string, TsdfNode>();
  const eligibleIndices: number[] = [];
  let skippedLowConfidence = 0;
  let rejectedComponentPoints = 0;
  for (let index = 0; index < sourcePointCount; index += 1) {
    const confidence = Number(input.confidence[index]);
    if (!Number.isFinite(confidence) || confidence < MIN_SURFACE_CONFIDENCE) {
      skippedLowConfidence += 1;
      continue;
    }
    if (index >= componentAnalysis.eligiblePointMask.length || componentAnalysis.eligiblePointMask[index] === 0) {
      rejectedComponentPoints += 1;
      continue;
    }
    eligibleIndices.push(index);
  }

  let integratedPointCount = 0;
  const stride = Math.max(1, Math.ceil(eligibleIndices.length / maxInputPoints));
  const radiusNodes = Math.ceil(truncationMeters / voxelSizeMeters);
  for (let cursor = 0; cursor < eligibleIndices.length; cursor += stride) {
    const index = eligibleIndices[cursor];
    const raw = sampleAt(input, index);
    if (raw === null || raw.confidence < MIN_SURFACE_CONFIDENCE) continue;
    const sample = orientTowardVerifiedFree(raw, freeIndex);
    integratedPointCount += 1;
    const cx = grid(sample.position[0], voxelSizeMeters);
    const cy = grid(sample.position[1], voxelSizeMeters);
    const cz = grid(sample.position[2], voxelSizeMeters);

    for (let dx = -radiusNodes; dx <= radiusNodes; dx += 1) {
      for (let dy = -radiusNodes; dy <= radiusNodes; dy += 1) {
        for (let dz = -radiusNodes; dz <= radiusNodes; dz += 1) {
          const ix = cx + dx;
          const iy = cy + dy;
          const iz = cz + dz;
          const nodePosition: Point3 = [
            ix * voxelSizeMeters,
            iy * voxelSizeMeters,
            iz * voxelSizeMeters,
          ];
          const delta = subtract(nodePosition, sample.position);
          const signedMeters = dot(sample.normal, delta);
          if (Math.abs(signedMeters) > truncationMeters) continue;
          const squaredDistance = dot(delta, delta);
          const tangentSquared = Math.max(0, squaredDistance - signedMeters * signedMeters);
          if (tangentSquared > tangentRadiusMeters * tangentRadiusMeters) continue;

          const tangentWeight = Math.exp(-0.5 * tangentSquared / (tangentSigmaMeters * tangentSigmaMeters));
          const truncationWeight = 1 - 0.30 * Math.abs(signedMeters) / truncationMeters;
          const observationWeight = sample.confidence * sample.confidence * tangentWeight * truncationWeight;
          if (observationWeight < 0.01) continue;
          const normalizedDistance = clamp(signedMeters / truncationMeters, -1, 1);
          const nodeKey = key(ix, iy, iz);
          const existing = nodes.get(nodeKey);
          if (existing === undefined) {
            if (nodes.size >= maxNodes) continue;
            nodes.set(nodeKey, {
              x: ix, y: iy, z: iz,
              weightedValue: normalizedDistance * observationWeight,
              weight: observationWeight,
            });
          } else {
            existing.weightedValue += normalizedDistance * observationWeight;
            existing.weight += observationWeight;
          }
        }
      }
    }
  }

  const triangles: number[] = [];
  let budgetReached = false;
  const minimumTriangleArea = voxelSizeMeters * voxelSizeMeters * MIN_TRIANGLE_AREA_FACTOR;
  outer: for (const origin of nodes.values()) {
    const cubeNodes: TsdfNode[] = [];
    const cubePositions: Point3[] = [];
    const cubeValues: number[] = [];
    let minimumWeight = Number.POSITIVE_INFINITY;
    for (const [ox, oy, oz] of CUBE_OFFSETS) {
      const candidate = nodes.get(key(origin.x + ox, origin.y + oy, origin.z + oz));
      if (candidate === undefined || candidate.weight < MIN_NODE_WEIGHT) continue outer;
      cubeNodes.push(candidate);
      cubePositions.push([
        (origin.x + ox) * voxelSizeMeters,
        (origin.y + oy) * voxelSizeMeters,
        (origin.z + oz) * voxelSizeMeters,
      ]);
      cubeValues.push(candidate.weightedValue / candidate.weight);
      minimumWeight = Math.min(minimumWeight, candidate.weight);
    }
    if (cubeNodes.length !== 8 || minimumWeight < MIN_NODE_WEIGHT) continue;
    let negative = false;
    let positive = false;
    for (const value of cubeValues) {
      if (value < 0) negative = true;
      else positive = true;
    }
    if (!negative || !positive) continue;

    for (const tetra of TETRAHEDRA) {
      const p: [Point3, Point3, Point3, Point3] = [
        cubePositions[tetra[0]], cubePositions[tetra[1]], cubePositions[tetra[2]], cubePositions[tetra[3]],
      ];
      const v: [number, number, number, number] = [
        cubeValues[tetra[0]], cubeValues[tetra[1]], cubeValues[tetra[2]], cubeValues[tetra[3]],
      ];
      for (const triangle of polygonizeTetra(p, v)) {
        if (triangleArea(triangle[0], triangle[1], triangle[2]) < minimumTriangleArea) continue;
        triangles.push(...triangle[0], ...triangle[1], ...triangle[2]);
        if (triangles.length / 9 >= maxTriangles) { budgetReached = true; break outer; }
      }
    }
  }

  const topology = sanitizeMeshTopology(Float32Array.from(triangles), voxelSizeMeters);
  const ended = typeof performance !== 'undefined' ? performance.now() : Date.now();
  const mesh: SparseTsdfMesh = {
    trianglePositions: topology.trianglePositions,
    triangleCount: topology.triangleCount,
    rawTriangleCount: topology.sourceTriangleCount,
    quarantinedTriangleCount: topology.quarantinedTriangleCount,
    topologyDiscardedTriangleCount: topology.discardedTriangleCount,
    nonManifoldEdgeCount: topology.nonManifoldEdgeCount,
    nonManifoldRepairTriangleCount: topology.nonManifoldRepairTriangleCount,
    orientationRepairCount: topology.orientationRepairCount,
    orientationConflictCount: topology.orientationConflictCount,
    meshTopologyComponentCount: topology.components.length,
    selfIntersectionPairCount: topology.selfIntersectionPairCount,
    selfIntersectionAffectedTriangleCount: topology.selfIntersectionAffectedTriangleCount,
    selfIntersectionExactTestedPairs: topology.selfIntersectionExactTestedPairs,
    selfIntersectionBroadPhaseCandidatePairs: topology.selfIntersectionBroadPhaseCandidatePairs,
    selfIntersectionTruncated: topology.selfIntersectionTruncated,
    selfIntersectionOversizedTriangleCount: topology.selfIntersectionOversizedTriangleCount,
    localNoiseHotspotCount: topology.localNoiseHotspotCount,
    localNoiseHotspotTriangleCount: topology.localNoiseHotspotTriangleCount,
    meshNoiseScore: topology.components.reduce((maximum, component) => Math.max(maximum, component.meshNoiseScore), 0),
    sourcePointCount,
    integratedPointCount,
    skippedLowConfidence,
    rejectedComponentPoints,
    componentCount: componentAnalysis.componentCount,
    eligibleComponentCount: componentAnalysis.eligibleComponentCount,
    meshReadiness: componentAnalysis.bestMeshComponent?.meshReadiness ?? 0,
    nodeCount: nodes.size,
    voxelSizeMeters,
    truncationMeters,
    computeMs: ended - started,
  };
  return { mesh, budgetReached };
}
