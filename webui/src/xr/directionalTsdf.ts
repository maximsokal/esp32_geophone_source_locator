import { soupVertexIds } from './meshIndex';
import { diagnoseMeshSelfIntersections, type MeshIntersectionTriangle } from './meshSelfIntersection';
import { sanitizeMeshTopology } from './meshTopologySanity';
import { analyzeMeshTopology, defaultMeshRefineOptions, refineScanMesh, type MeshTopologyStats } from './meshRefine';
import {
  buildSparseTsdfMesh,
  type Point3,
  type SparseTsdfInput,
  type SparseTsdfMesh,
} from './sparseTsdf';

export interface MeshRefineSummary { holesFilled: number; holeTrianglesAdded: number; openLoopsLeft: number; smoothedVertices: number; maxDisplacementMm: number }

export interface DirectionalTsdfMesh extends SparseTsdfMesh {
  /** The topology of the mesh as the viewer shows it, measured after the last operation that changes geometry. */
  finalTopology?: MeshTopologyStats;
  /** Points the scalar pass left without surface, handed to the directional pass (hybrid reconstruction); 0 when the scalar surface covered everything. */
  directionalFallbackPoints?: number;
  /** What the final cosmetic pass did (small holes closed, a little smoothing); absent when it was not run. */
  meshRefine?: MeshRefineSummary;
  directionalChannelCount: number;
  channelTriangleCounts: readonly number[];
}

export type DirectionalTsdfPlanMode = 'scalar' | 'single' | 'multi' | 'base-only';

export interface DirectionalTsdfPlanSummary {
  mode: DirectionalTsdfPlanMode;
  sourcePointCount: number;
  skippedLowConfidence: number;
  rejectedUndersizedChannelPoints: number;
  totalTriangleBudget: number;
  inputVoxelSizeMeters: number;
  directionalChannelCount: number;
  /** Hybrid reconstruction: the scalar surface already built from all the points; the channels only add what it did not cover. */
  baseMesh?: SparseTsdfMesh;
  directionalFallbackPoints?: number;
}

export interface DirectionalTsdfPlan extends DirectionalTsdfPlanSummary {
  channelInputs: readonly SparseTsdfInput[];
}

type Channel = 0 | 1 | 2 | 3 | 4 | 5;

interface FreeIndex {
  size: number;
  buckets: Map<string, Point3[]>;
}

const MIN_DIRECTIONAL_CHANNEL_POINTS = 24;
const MIN_DIRECTIONAL_CONFIDENCE = 0.58;

function key(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }

function buildFreeIndex(input: SparseTsdfInput): FreeIndex | null {
  const positions = input.freeSpacePositions;
  const requestedSize = input.freeSpaceVoxelSizeMeters;
  if (
    positions === undefined || positions.length < 3 ||
    requestedSize === undefined || !Number.isFinite(requestedSize) || requestedSize <= 0
  ) return null;
  // This must stay bit-for-bit policy-equivalent to sparseTsdf.buildFreeIndex().
  // Transfer-safe directional channels omit FREE after this planner has oriented
  // their normals, so any difference here could change which side becomes the
  // positive TSDF side in parallel reconstruction.
  const size = Math.max(requestedSize, 0.04);
  const buckets = new Map<string, Point3[]>();
  for (let offset = 0; offset + 2 < positions.length; offset += 3) {
    const point: Point3 = [positions[offset], positions[offset + 1], positions[offset + 2]];
    if (!point.every(Number.isFinite)) continue;
    const bucketKey = key(
      Math.floor(point[0] / size),
      Math.floor(point[1] / size),
      Math.floor(point[2] / size),
    );
    const bucket = buckets.get(bucketKey) ?? [];
    bucket.push(point);
    buckets.set(bucketKey, bucket);
  }
  return buckets.size > 0 ? { size, buckets } : null;
}

function orientedNormal(
  px: number, py: number, pz: number,
  nx: number, ny: number, nz: number,
  free: FreeIndex | null,
): readonly [number, number, number] {
  const length = Math.hypot(nx, ny, nz) || 1;
  let x = nx / length, y = ny / length, z = nz / length;
  if (free !== null) {
    const gx = Math.floor(px / free.size), gy = Math.floor(py / free.size), gz = Math.floor(pz / free.size);
    let bestSquared = (free.size * 3.5) ** 2;
    let best: readonly [number, number, number] | null = null;
    // Match sparseTsdf.nearestFreeDirection(): the planner may only pre-orient
    // from FREE cells that the scalar channel would itself have considered.
    for (let dx = -2; dx <= 2; dx += 1) {
      for (let dy = -2; dy <= 2; dy += 1) {
        for (let dz = -2; dz <= 2; dz += 1) {
          const bucket = free.buckets.get(key(gx + dx, gy + dy, gz + dz));
          if (bucket === undefined) continue;
          for (const point of bucket) {
            const fx = point[0] - px, fy = point[1] - py, fz = point[2] - pz;
            const squared = fx * fx + fy * fy + fz * fz;
            if (squared < bestSquared) { bestSquared = squared; best = [fx, fy, fz]; }
          }
        }
      }
    }
    if (best !== null && x * best[0] + y * best[1] + z * best[2] < 0) { x = -x; y = -y; z = -z; }
  }
  return [x, y, z];
}

function channelFor(normal: readonly [number, number, number]): Channel {
  const ax = Math.abs(normal[0]), ay = Math.abs(normal[1]), az = Math.abs(normal[2]);
  if (ax >= ay && ax >= az) return normal[0] >= 0 ? 0 : 1;
  if (ay >= az) return normal[1] >= 0 ? 2 : 3;
  return normal[2] >= 0 ? 4 : 5;
}

function subsetInput(
  input: SparseTsdfInput,
  indices: readonly number[],
  normalsByIndex: Map<number, readonly [number, number, number]>,
  keepFreeSpace: boolean,
): SparseTsdfInput {
  const positions = new Float32Array(indices.length * 3);
  const normals = new Float32Array(indices.length * 3);
  const confidence = new Float32Array(indices.length);
  indices.forEach((source, target) => {
    const so = source * 3, to = target * 3;
    positions[to] = input.positions[so]; positions[to + 1] = input.positions[so + 1]; positions[to + 2] = input.positions[so + 2];
    const normal = normalsByIndex.get(source)!;
    normals[to] = normal[0]; normals[to + 1] = normal[1]; normals[to + 2] = normal[2];
    confidence[target] = input.confidence[source];
  });
  return {
    ...input,
    positions,
    normals,
    confidence,
    freeSpacePositions: keepFreeSpace ? input.freeSpacePositions : undefined,
    freeSpaceVoxelSizeMeters: keepFreeSpace ? input.freeSpaceVoxelSizeMeters : undefined,
    maxInputPoints: Math.min(indices.length, input.maxInputPoints ?? indices.length),
  };
}

function triangleKey(values: Float32Array, offset: number, quantization: number): string {
  const vertices: string[] = [];
  for (let vertex = 0; vertex < 3; vertex += 1) {
    const base = offset + vertex * 3;
    vertices.push(`${Math.round(values[base] / quantization)},${Math.round(values[base + 1] / quantization)},${Math.round(values[base + 2] / quantization)}`);
  }
  vertices.sort();
  return vertices.join('|');
}

function maximumMeshReadiness(meshes: readonly SparseTsdfMesh[]): number {
  let maximum = 0;
  for (const mesh of meshes) maximum = Math.max(maximum, mesh.meshReadiness);
  return maximum;
}

/** A point the scalar surface leaves without mesh within this many voxels is handed to the directional pass. */
const HYBRID_COVERAGE_VOXELS = 1.25;
export const HYBRID_MIN_UNCOVERED_POINTS = 60;
export const HYBRID_MIN_EXTRA_TRIANGLES = 200;

/** Indices of the points that have no mesh sample within `radius`. */
function uncoveredPointIndices(input: SparseTsdfInput, soup: Float32Array, radius: number): number[] {
  const count = Math.min(Math.floor(input.positions.length / 3), Math.floor(input.normals.length / 3), input.confidence.length);
  const grid = new Map<string, number[]>();
  const add = (x: number, y: number, z: number): void => {
    const k = `${Math.floor(x / radius)},${Math.floor(y / radius)},${Math.floor(z / radius)}`;
    const list = grid.get(k);
    if (list) list.push(x, y, z); else grid.set(k, [x, y, z]);
  };
  for (let t = 0; t + 8 < soup.length; t += 9) {
    for (const [a, b, c] of MESH_SAMPLE_WEIGHTS) {
      add(a * soup[t] + b * soup[t + 3] + c * soup[t + 6], a * soup[t + 1] + b * soup[t + 4] + c * soup[t + 7], a * soup[t + 2] + b * soup[t + 5] + c * soup[t + 8]);
    }
  }
  const uncovered: number[] = [];
  for (let i = 0; i < count; i += 1) {
    const x = input.positions[i * 3]; const y = input.positions[i * 3 + 1]; const z = input.positions[i * 3 + 2];
    if (![x, y, z].every(Number.isFinite)) continue;
    const gx = Math.floor(x / radius); const gy = Math.floor(y / radius); const gz = Math.floor(z / radius);
    let near = false;
    for (let dx = -1; dx <= 1 && !near; dx += 1) for (let dy = -1; dy <= 1 && !near; dy += 1) for (let dz = -1; dz <= 1 && !near; dz += 1) {
      const list = grid.get(`${gx + dx},${gy + dy},${gz + dz}`);
      if (list === undefined) continue;
      for (let k = 0; k < list.length; k += 3) if (Math.hypot(list[k] - x, list[k + 1] - y, list[k + 2] - z) <= radius) { near = true; break; }
    }
    if (!near) uncovered.push(i);
  }
  return uncovered;
}
const MESH_SAMPLE_WEIGHTS: ReadonlyArray<readonly [number, number, number]> = [[1 / 3, 1 / 3, 1 / 3], [1, 0, 0], [0, 1, 0], [0, 0, 1], [.5, .5, 0], [0, .5, .5], [.5, 0, .5]];

/**
 * Hybrid reconstruction (measured on public scenes and on the real scan of 2026-11-26): ONE scalar TSDF over all the points builds the surface; the
 * six-channel directional pass then runs ONLY on the points that surface left uncovered. The scalar field is robust on curved and noisy geometry,
 * where the channel-by-channel mesh collapsed to zero triangles (a sphere, two objects, a noisy corner) and produced hundreds of self-intersections;
 * the directional pass is what keeps a thin board measured from both sides (opposite normals cancel in a scalar field). Each is used where it is right.
 */
export function prepareHybridTsdfPlan(input: SparseTsdfInput, transferSafeChannels = false): DirectionalTsdfPlan {
  const count = Math.min(Math.floor(input.positions.length / 3), Math.floor(input.normals.length / 3), input.confidence.length);
  const totalTriangleBudget = Math.max(1, Math.floor(input.maxTriangles ?? 50000));
  const base = buildSparseTsdfMesh({ ...input, maxTriangles: totalTriangleBudget });
  const radius = Math.max(0.05, Math.abs(input.voxelSizeMeters) * HYBRID_COVERAGE_VOXELS);
  const uncovered = uncoveredPointIndices(input, base.trianglePositions, radius);
  /* The budget is one budget: what the scalar surface left, the directional pass may only add within what is left of it. */
  const remainingBudget = totalTriangleBudget - base.triangleCount;
  if (uncovered.length < HYBRID_MIN_UNCOVERED_POINTS || remainingBudget < HYBRID_MIN_EXTRA_TRIANGLES) {
    return {
      mode: 'base-only', sourcePointCount: count, skippedLowConfidence: base.skippedLowConfidence, rejectedUndersizedChannelPoints: 0, totalTriangleBudget,
      inputVoxelSizeMeters: input.voxelSizeMeters, directionalChannelCount: 0, baseMesh: base, directionalFallbackPoints: uncovered.length, channelInputs: [],
    };
  }
  const positions = new Float32Array(uncovered.length * 3); const normals = new Float32Array(uncovered.length * 3); const confidence = new Float32Array(uncovered.length);
  uncovered.forEach((source, target) => {
    positions.set(input.positions.subarray(source * 3, source * 3 + 3), target * 3);
    normals.set(input.normals.subarray(source * 3, source * 3 + 3), target * 3);
    confidence[target] = input.confidence[source];
  });
  const plan = prepareDirectionalTsdfPlan({
    ...input, positions, normals, confidence, maxInputPoints: Math.min(uncovered.length, input.maxInputPoints ?? uncovered.length),
    maxTriangles: remainingBudget,
  }, transferSafeChannels);
  return { ...plan, sourcePointCount: count, baseMesh: base, directionalFallbackPoints: uncovered.length };
}

/**
 * Partition one post-scan physical surfel cloud into signed dominant-normal
 * families. The work here is deterministic and intentionally independent of
 * Worker scheduling, so sequential and multi-worker reconstruction share the
 * same geometry policy.
 *
 * When `transferSafeChannels` is true, mature channel inputs omit the shared
 * FREE-space buffer. Their normals have already been oriented against that exact
 * FREE evidence here, avoiding N copies of a potentially large free-space array
 * when channel buffers are transferred to independent Workers.
 */
export function prepareDirectionalTsdfPlan(
  input: SparseTsdfInput,
  transferSafeChannels = false,
): DirectionalTsdfPlan {
  const count = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
  );
  const free = buildFreeIndex(input);
  const channels: number[][] = Array.from({ length: 6 }, () => []);
  const normalsByIndex = new Map<number, readonly [number, number, number]>();
  let skippedLowConfidence = 0;
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    if (input.confidence[index] < MIN_DIRECTIONAL_CONFIDENCE) {
      skippedLowConfidence += 1;
      continue;
    }
    const normal = orientedNormal(
      input.positions[offset], input.positions[offset + 1], input.positions[offset + 2],
      input.normals[offset], input.normals[offset + 1], input.normals[offset + 2],
      free,
    );
    normalsByIndex.set(index, normal);
    channels[channelFor(normal)].push(index);
  }

  const totalTriangleBudget = Math.max(1, Math.floor(input.maxTriangles ?? 50000));
  const active = channels.filter((items) => items.length >= MIN_DIRECTIONAL_CHANNEL_POINTS);
  const highConfidencePointCount = count - skippedLowConfidence;
  const activePointCount = active.reduce((sum, items) => sum + items.length, 0);
  const rejectedUndersizedChannelPoints = Math.max(0, highConfidencePointCount - activePointCount);

  if (active.length === 0) {
    return {
      mode: 'scalar',
      sourcePointCount: count,
      skippedLowConfidence,
      rejectedUndersizedChannelPoints,
      totalTriangleBudget,
      inputVoxelSizeMeters: input.voxelSizeMeters,
      directionalChannelCount: 0,
      channelInputs: [input],
    };
  }

  if (active.length === 1) {
    return {
      mode: 'single',
      sourcePointCount: count,
      skippedLowConfidence,
      rejectedUndersizedChannelPoints,
      totalTriangleBudget,
      inputVoxelSizeMeters: input.voxelSizeMeters,
      directionalChannelCount: 1,
      channelInputs: [subsetInput(input, active[0], normalsByIndex, !transferSafeChannels)],
    };
  }

  /*
   * The triangle budget follows the points of each channel. An equal split gave the up-facing channel (the floor, most of a typical scan) the same 1/6
   * as a channel with a few dozen wall points: on the 2026-11-22 scan the floor mesh stopped at its 3,000-triangle share after about a quarter of the
   * floor, the rest of it simply missing. Every channel keeps a floor of 32 triangles.
   */
  const budgetFor = (points: number): number => Math.max(32, Math.floor(totalTriangleBudget * points / Math.max(1, activePointCount)));
  return {
    mode: 'multi',
    sourcePointCount: count,
    skippedLowConfidence,
    rejectedUndersizedChannelPoints,
    totalTriangleBudget,
    inputVoxelSizeMeters: input.voxelSizeMeters,
    directionalChannelCount: active.length,
    channelInputs: active.map((indices) => ({
      ...subsetInput(input, indices, normalsByIndex, !transferSafeChannels),
      maxTriangles: budgetFor(indices.length),
    })),
  };
}

/**
 * Merge already-sanitized channel meshes and run one final cross-channel
 * topology pass. This function contains no channel reconstruction work, which
 * lets post-scan callers execute mature channels on multiple CPU Workers while
 * preserving the same final topology contract as the sequential fallback.
 */
function summaryOf(refined: ReturnType<typeof refineScanMesh>): MeshRefineSummary {
  return {
    holesFilled: refined.holesFilled, holeTrianglesAdded: refined.holeTrianglesAdded, openLoopsLeft: refined.openLoopsLeft,
    smoothedVertices: refined.smoothedVertices, maxDisplacementMm: Math.round(refined.maxDisplacementMeters * 10000) / 10,
  };
}

/** A scalar or single-channel result as a directional mesh, before the final refinement. */
function asDirectional(mesh: SparseTsdfMesh): DirectionalTsdfMesh {
  return { ...mesh, directionalChannelCount: 0, channelTriangleCounts: [mesh.triangleCount] };
}

function finalizeChannels(
  plan: DirectionalTsdfPlanSummary,
  meshes: readonly SparseTsdfMesh[],
  totalComputeMs?: number,
): DirectionalTsdfMesh {
  if (meshes.length === 0) throw new Error('Directional TSDF finalization requires at least one channel mesh.');

  if (plan.mode === 'scalar') {
    const scalar = asDirectional(meshes[0]);
    return { ...scalar, directionalChannelCount: 0, channelTriangleCounts: [scalar.triangleCount] };
  }

  if (plan.mode === 'single') {
    const scalar = asDirectional(meshes[0]);
    return {
      ...scalar,
      sourcePointCount: plan.sourcePointCount,
      skippedLowConfidence: plan.skippedLowConfidence,
      rejectedComponentPoints: scalar.rejectedComponentPoints + plan.rejectedUndersizedChannelPoints,
      directionalChannelCount: 1,
      channelTriangleCounts: [scalar.triangleCount],
    };
  }

  if (meshes.length !== plan.directionalChannelCount) {
    throw new Error(`Directional TSDF expected ${plan.directionalChannelCount} channel meshes, received ${meshes.length}.`);
  }

  const combined: number[] = [];
  const seen = new Set<string>();
  const quantization = Math.max(0.002, (meshes[0]?.voxelSizeMeters ?? plan.inputVoxelSizeMeters) * 0.10);
  outer: for (const mesh of meshes) {
    for (let offset = 0; offset + 8 < mesh.trianglePositions.length; offset += 9) {
      const signature = triangleKey(mesh.trianglePositions, offset, quantization);
      if (seen.has(signature)) continue;
      seen.add(signature);
      for (let index = 0; index < 9; index += 1) combined.push(mesh.trianglePositions[offset + index]);
      if (combined.length / 9 >= plan.totalTriangleBudget) break outer;
    }
  }

  const voxelSizeMeters = meshes[0]?.voxelSizeMeters ?? plan.inputVoxelSizeMeters;
  const finalTopology = sanitizeMeshTopology(Float32Array.from(combined), voxelSizeMeters);
  const channelHotspotCount = meshes.reduce((sum, mesh) => sum + mesh.localNoiseHotspotCount, 0);
  const channelHotspotTriangles = meshes.reduce((sum, mesh) => sum + mesh.localNoiseHotspotTriangleCount, 0);
  return {
    trianglePositions: finalTopology.trianglePositions,
    triangleCount: finalTopology.triangleCount,
    rawTriangleCount: finalTopology.sourceTriangleCount,
    quarantinedTriangleCount:
      finalTopology.quarantinedTriangleCount +
      meshes.reduce((sum, mesh) => sum + mesh.quarantinedTriangleCount, 0),
    topologyDiscardedTriangleCount:
      finalTopology.discardedTriangleCount +
      meshes.reduce((sum, mesh) => sum + mesh.topologyDiscardedTriangleCount, 0),
    nonManifoldEdgeCount:
      finalTopology.nonManifoldEdgeCount +
      meshes.reduce((sum, mesh) => sum + mesh.nonManifoldEdgeCount, 0),
    nonManifoldRepairTriangleCount:
      finalTopology.nonManifoldRepairTriangleCount +
      meshes.reduce((sum, mesh) => sum + mesh.nonManifoldRepairTriangleCount, 0),
    orientationRepairCount:
      finalTopology.orientationRepairCount +
      meshes.reduce((sum, mesh) => sum + mesh.orientationRepairCount, 0),
    orientationConflictCount:
      finalTopology.orientationConflictCount +
      meshes.reduce((sum, mesh) => sum + mesh.orientationConflictCount, 0),
    meshTopologyComponentCount: finalTopology.components.length,
    selfIntersectionPairCount: finalTopology.selfIntersectionPairCount,
    selfIntersectionAffectedTriangleCount: finalTopology.selfIntersectionAffectedTriangleCount,
    selfIntersectionExactTestedPairs: finalTopology.selfIntersectionExactTestedPairs,
    selfIntersectionBroadPhaseCandidatePairs: finalTopology.selfIntersectionBroadPhaseCandidatePairs,
    selfIntersectionTruncated: finalTopology.selfIntersectionTruncated,
    selfIntersectionOversizedTriangleCount: finalTopology.selfIntersectionOversizedTriangleCount,
    localNoiseHotspotCount: finalTopology.localNoiseHotspotCount + channelHotspotCount,
    localNoiseHotspotTriangleCount: finalTopology.localNoiseHotspotTriangleCount + channelHotspotTriangles,
    meshNoiseScore: finalTopology.components.reduce((maximum, component) => Math.max(maximum, component.meshNoiseScore), 0),
    sourcePointCount: plan.sourcePointCount,
    integratedPointCount: meshes.reduce((sum, mesh) => sum + mesh.integratedPointCount, 0),
    skippedLowConfidence: plan.skippedLowConfidence,
    rejectedComponentPoints: plan.rejectedUndersizedChannelPoints + meshes.reduce((sum, mesh) => sum + mesh.rejectedComponentPoints, 0),
    componentCount: meshes.reduce((sum, mesh) => sum + mesh.componentCount, 0),
    eligibleComponentCount: meshes.reduce((sum, mesh) => sum + mesh.eligibleComponentCount, 0),
    meshReadiness: maximumMeshReadiness(meshes),
    coarsenings: meshes.reduce((maximum, mesh) => Math.max(maximum, mesh.coarsenings ?? 0), 0),
    nodeCount: meshes.reduce((sum, mesh) => sum + mesh.nodeCount, 0),
    voxelSizeMeters,
    truncationMeters: meshes[0]?.truncationMeters ?? plan.inputVoxelSizeMeters * 2.75,
    computeMs: totalComputeMs ?? meshes.reduce((sum, mesh) => sum + mesh.computeMs, 0),
    directionalChannelCount: plan.directionalChannelCount,
    channelTriangleCounts: meshes.map((mesh) => mesh.triangleCount),
  };
}

/** The base surface and the directional additions as one soup (they cover different points, so nothing is deduplicated), counts added. */
function combineBaseAndChannels(base: SparseTsdfMesh, extra: DirectionalTsdfMesh | null, plan: DirectionalTsdfPlanSummary): DirectionalTsdfMesh {
  if (extra === null) return { ...asDirectional(base), sourcePointCount: plan.sourcePointCount, directionalFallbackPoints: plan.directionalFallbackPoints ?? 0 };
  const soup = new Float32Array(base.trianglePositions.length + extra.trianglePositions.length);
  soup.set(base.trianglePositions, 0); soup.set(extra.trianglePositions, base.trianglePositions.length);
  return {
    ...extra,
    trianglePositions: soup, triangleCount: base.triangleCount + extra.triangleCount, rawTriangleCount: base.rawTriangleCount + extra.rawTriangleCount,
    quarantinedTriangleCount: base.quarantinedTriangleCount + extra.quarantinedTriangleCount,
    topologyDiscardedTriangleCount: base.topologyDiscardedTriangleCount + extra.topologyDiscardedTriangleCount,
    nonManifoldEdgeCount: base.nonManifoldEdgeCount + extra.nonManifoldEdgeCount,
    nonManifoldRepairTriangleCount: base.nonManifoldRepairTriangleCount + extra.nonManifoldRepairTriangleCount,
    orientationRepairCount: base.orientationRepairCount + extra.orientationRepairCount,
    orientationConflictCount: base.orientationConflictCount + extra.orientationConflictCount,
    meshTopologyComponentCount: base.meshTopologyComponentCount + extra.meshTopologyComponentCount,
    selfIntersectionPairCount: base.selfIntersectionPairCount + extra.selfIntersectionPairCount,
    selfIntersectionAffectedTriangleCount: base.selfIntersectionAffectedTriangleCount + extra.selfIntersectionAffectedTriangleCount,
    localNoiseHotspotCount: base.localNoiseHotspotCount + extra.localNoiseHotspotCount,
    localNoiseHotspotTriangleCount: base.localNoiseHotspotTriangleCount + extra.localNoiseHotspotTriangleCount,
    meshNoiseScore: Math.max(base.meshNoiseScore, extra.meshNoiseScore),
    sourcePointCount: plan.sourcePointCount,
    integratedPointCount: base.integratedPointCount + extra.integratedPointCount,
    componentCount: base.componentCount + extra.componentCount,
    eligibleComponentCount: base.eligibleComponentCount + extra.eligibleComponentCount,
    nodeCount: base.nodeCount + extra.nodeCount,
    computeMs: base.computeMs + extra.computeMs,
    coarsenings: Math.max(base.coarsenings ?? 0, extra.coarsenings ?? 0),
    directionalFallbackPoints: plan.directionalFallbackPoints ?? 0,
  };
}

/** The last operation that changes geometry, then the measurement of what is left: the diagnostics describe the displayed mesh. */
function refineAndMeasure(mesh: DirectionalTsdfMesh): DirectionalTsdfMesh {
  const refined = refineScanMesh(mesh.trianglePositions, mesh.voxelSizeMeters);
  const weld = defaultMeshRefineOptions(mesh.voxelSizeMeters).weldMeters;
  const topology = analyzeMeshTopology(refined.trianglePositions, weld);
  /* The stitching changes the geometry, so every reported defect figure is measured again, on the triangles that are displayed. */
  const selfIntersection = measureSelfIntersections(refined.trianglePositions, mesh.voxelSizeMeters);
  return {
    ...mesh,
    trianglePositions: refined.trianglePositions, triangleCount: refined.triangleCount, meshRefine: summaryOf(refined), finalTopology: topology,
    meshTopologyComponentCount: topology.components,
    nonManifoldEdgeCount: topology.nonManifoldEdges,
    orientationConflictCount: topology.orientationConflicts,
    selfIntersectionPairCount: selfIntersection.intersectingPairCount,
    selfIntersectionAffectedTriangleCount: selfIntersection.affectedTriangleCount,
    selfIntersectionExactTestedPairs: selfIntersection.exactTestedPairs,
    selfIntersectionBroadPhaseCandidatePairs: selfIntersection.broadPhaseCandidatePairs,
    selfIntersectionOversizedTriangleCount: selfIntersection.oversizedTriangleCount,
    /* also stays set when the earlier, pre-stitching screening of the components could not finish its checks */
    selfIntersectionTruncated: selfIntersection.truncated || mesh.selfIntersectionTruncated,
  };
}

/** The complete self-intersection check of a finished soup. */
export function measureSelfIntersections(soup: Float32Array, voxelSizeMeters: number) {
  const ids = soupVertexIds(soup);
  const triangles: MeshIntersectionTriangle[] = [];
  for (let t = 0, offset = 0; offset + 8 < soup.length; t += 1, offset += 9) {
    triangles.push({
      vertices: [[soup[offset], soup[offset + 1], soup[offset + 2]], [soup[offset + 3], soup[offset + 4], soup[offset + 5]], [soup[offset + 6], soup[offset + 7], soup[offset + 8]]],
      keys: [ids[t * 3], ids[t * 3 + 1], ids[t * 3 + 2]],
    });
  }
  return diagnoseMeshSelfIntersections(triangles, new Int32Array(triangles.length), 1, voxelSizeMeters);
}

export function finalizeDirectionalTsdfPlan(
  plan: DirectionalTsdfPlanSummary,
  meshes: readonly SparseTsdfMesh[],
  totalComputeMs?: number,
): DirectionalTsdfMesh {
  const base = plan.baseMesh;
  if (base === undefined) return refineAndMeasure(finalizeChannels(plan, meshes, totalComputeMs));
  const extra = meshes.length === 0 ? null : finalizeChannels(plan, meshes, totalComputeMs);
  return refineAndMeasure(combineBaseAndChannels(base, extra, plan));
}

/**
 * Directional TSDF wrapper: locally opposite/orthogonal surfaces never share a
 * scalar field. Each signed dominant-normal family is fused independently and
 * resulting triangles are deduplicated only after zero-crossing extraction.
 *
 * A directional channel must contain at least the same 24 physical samples that
 * a connected surface component needs before mesh eligibility. Smaller normal
 * islands are not worth allocating a TSDF field and are counted as rejected
 * component evidence rather than being allowed to contaminate a mature channel.
 *
 * Per-channel meshes are sanitized independently by buildSparseTsdfMesh. After
 * channel merge we sanitize once again: valid faces from two channels can still
 * overlap or create a non-manifold edge at a directional boundary.
 */
export function buildDirectionalTsdfMesh(input: SparseTsdfInput, options: { hybrid?: boolean } = {}): DirectionalTsdfMesh {
  const started = typeof performance !== 'undefined' ? performance.now() : Date.now();
  const plan = options.hybrid === false ? prepareDirectionalTsdfPlan(input, false) : prepareHybridTsdfPlan(input, false);
  const meshes = plan.channelInputs.map((channelInput) => buildSparseTsdfMesh(channelInput));
  const elapsed = (typeof performance !== 'undefined' ? performance.now() : Date.now()) - started;
  return finalizeDirectionalTsdfPlan(plan, meshes, plan.mode === 'multi' ? elapsed : undefined);
}
