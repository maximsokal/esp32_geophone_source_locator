import { sanitizeMeshTopology } from './meshTopologySanity';
import {
  buildSparseTsdfMesh,
  type Point3,
  type SparseTsdfInput,
  type SparseTsdfMesh,
} from './sparseTsdf';

export interface DirectionalTsdfMesh extends SparseTsdfMesh {
  directionalChannelCount: number;
  channelTriangleCounts: readonly number[];
}

export type DirectionalTsdfPlanMode = 'scalar' | 'single' | 'multi';

export interface DirectionalTsdfPlanSummary {
  mode: DirectionalTsdfPlanMode;
  sourcePointCount: number;
  skippedLowConfidence: number;
  rejectedUndersizedChannelPoints: number;
  totalTriangleBudget: number;
  inputVoxelSizeMeters: number;
  directionalChannelCount: number;
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

  const perChannelBudget = Math.max(32, Math.floor(totalTriangleBudget / active.length));
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
      maxTriangles: perChannelBudget,
    })),
  };
}

/**
 * Merge already-sanitized channel meshes and run one final cross-channel
 * topology pass. This function contains no channel reconstruction work, which
 * lets post-scan callers execute mature channels on multiple CPU Workers while
 * preserving the same final topology contract as the sequential fallback.
 */
export function finalizeDirectionalTsdfPlan(
  plan: DirectionalTsdfPlanSummary,
  meshes: readonly SparseTsdfMesh[],
  totalComputeMs?: number,
): DirectionalTsdfMesh {
  if (meshes.length === 0) throw new Error('Directional TSDF finalization requires at least one channel mesh.');

  if (plan.mode === 'scalar') {
    const scalar = meshes[0];
    return { ...scalar, directionalChannelCount: 0, channelTriangleCounts: [scalar.triangleCount] };
  }

  if (plan.mode === 'single') {
    const scalar = meshes[0];
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
    nodeCount: meshes.reduce((sum, mesh) => sum + mesh.nodeCount, 0),
    voxelSizeMeters,
    truncationMeters: meshes[0]?.truncationMeters ?? plan.inputVoxelSizeMeters * 2.75,
    computeMs: totalComputeMs ?? meshes.reduce((sum, mesh) => sum + mesh.computeMs, 0),
    directionalChannelCount: plan.directionalChannelCount,
    channelTriangleCounts: meshes.map((mesh) => mesh.triangleCount),
  };
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
export function buildDirectionalTsdfMesh(input: SparseTsdfInput): DirectionalTsdfMesh {
  const started = typeof performance !== 'undefined' ? performance.now() : Date.now();
  const plan = prepareDirectionalTsdfPlan(input, false);
  const meshes = plan.channelInputs.map((channelInput) => buildSparseTsdfMesh(channelInput));
  const elapsed = (typeof performance !== 'undefined' ? performance.now() : Date.now()) - started;
  return finalizeDirectionalTsdfPlan(plan, meshes, plan.mode === 'multi' ? elapsed : undefined);
}
