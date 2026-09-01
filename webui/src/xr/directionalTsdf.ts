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

type Channel = 0 | 1 | 2 | 3 | 4 | 5;

interface FreeIndex {
  size: number;
  buckets: Map<string, Point3[]>;
}

function key(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }

function buildFreeIndex(input: SparseTsdfInput): FreeIndex | null {
  const positions = input.freeSpacePositions;
  const size = input.freeSpaceVoxelSizeMeters;
  if (positions === undefined || positions.length < 3 || size === undefined || !Number.isFinite(size) || size <= 0) return null;
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
    for (let dx = -3; dx <= 3; dx += 1) {
      for (let dy = -3; dy <= 3; dy += 1) {
        for (let dz = -3; dz <= 3; dz += 1) {
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

/**
 * Directional TSDF wrapper: locally opposite/orthogonal surfaces never share a
 * scalar field. Each signed dominant-normal family is fused independently and
 * resulting triangles are deduplicated only after zero-crossing extraction.
 */
export function buildDirectionalTsdfMesh(input: SparseTsdfInput): DirectionalTsdfMesh {
  const started = typeof performance !== 'undefined' ? performance.now() : Date.now();
  const count = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
  );
  const free = buildFreeIndex(input);
  const channels: number[][] = Array.from({ length: 6 }, () => []);
  const normalsByIndex = new Map<number, readonly [number, number, number]>();
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    if (input.confidence[index] < 0.58) continue;
    const normal = orientedNormal(
      input.positions[offset], input.positions[offset + 1], input.positions[offset + 2],
      input.normals[offset], input.normals[offset + 1], input.normals[offset + 2],
      free,
    );
    normalsByIndex.set(index, normal);
    channels[channelFor(normal)].push(index);
  }

  const totalBudget = Math.max(1, Math.floor(input.maxTriangles ?? 50000));
  const active = channels.filter((items) => items.length >= 12);
  if (active.length <= 1) {
    const scalar = buildSparseTsdfMesh(input);
    return { ...scalar, directionalChannelCount: active.length, channelTriangleCounts: [scalar.triangleCount] };
  }

  const meshes: SparseTsdfMesh[] = [];
  const perChannelBudget = Math.max(32, Math.floor(totalBudget / active.length));
  for (const indices of active) {
    meshes.push(buildSparseTsdfMesh({
      ...subsetInput(input, indices, normalsByIndex),
      maxTriangles: perChannelBudget,
    }));
  }

  const combined: number[] = [];
  const seen = new Set<string>();
  const quantization = Math.max(0.002, (meshes[0]?.voxelSizeMeters ?? input.voxelSizeMeters) * 0.10);
  outer: for (const mesh of meshes) {
    for (let offset = 0; offset + 8 < mesh.trianglePositions.length; offset += 9) {
      const signature = triangleKey(mesh.trianglePositions, offset, quantization);
      if (seen.has(signature)) continue;
      seen.add(signature);
      for (let index = 0; index < 9; index += 1) combined.push(mesh.trianglePositions[offset + index]);
      if (combined.length / 9 >= totalBudget) break outer;
    }
  }

  const elapsed = (typeof performance !== 'undefined' ? performance.now() : Date.now()) - started;
  return {
    trianglePositions: Float32Array.from(combined),
    triangleCount: Math.floor(combined.length / 9),
    sourcePointCount: count,
    integratedPointCount: meshes.reduce((sum, mesh) => sum + mesh.integratedPointCount, 0),
    skippedLowConfidence: meshes.reduce((sum, mesh) => sum + mesh.skippedLowConfidence, 0),
    nodeCount: meshes.reduce((sum, mesh) => sum + mesh.nodeCount, 0),
    voxelSizeMeters: meshes[0]?.voxelSizeMeters ?? input.voxelSizeMeters,
    truncationMeters: meshes[0]?.truncationMeters ?? input.voxelSizeMeters * 2.75,
    computeMs: elapsed,
    directionalChannelCount: active.length,
    channelTriangleCounts: meshes.map((mesh) => mesh.triangleCount),
  };
}
