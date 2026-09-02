export interface SurfaceBridgeEvidenceInput {
  positions: ArrayLike<number>;
  componentIdByPoint: ArrayLike<number>;
  retainedComponentIds: ReadonlySet<number>;
  quarantinedComponentIds: ReadonlySet<number>;
  voxelSizeMeters: number;
}

export interface SurfaceBridgeEvidenceResult {
  rescanComponentIds: readonly number[];
  weakClusterCount: number;
  bridgeClusterCount: number;
  examinedWeakPointCount: number;
  neighbourChecks: number;
  truncated: boolean;
}

const MIN_WEAK_BRIDGE_POINTS = 2;
const MAX_WEAK_POINTS = 6000;
const MAX_NEIGHBOUR_CHECKS = 90_000;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function cellKey(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

/**
 * Find advisory evidence that a quarantined physical island may be separated from
 * retained structure by a noisy/under-observed strip rather than by real empty air.
 *
 * Only source surfels that the connected-component pass left unassigned
 * (`componentId < 0`) are allowed to form a weak bridge cluster. A cluster must
 * contain multiple measured weak points and physically touch both a retained and a
 * quarantined component inside a voxel-scaled metric radius. The result is RESCAN
 * metadata only: it never relabels points, expands UNKNOWN, modifies occupancy or
 * certifies a mesh. This makes false positives operationally cheap and false
 * geometry impossible.
 */
export function detectWeakSurfaceBridgeEvidence(
  input: SurfaceBridgeEvidenceInput,
): SurfaceBridgeEvidenceResult {
  const pointCount = Math.min(
    Math.floor(input.positions.length / 3),
    input.componentIdByPoint.length,
  );
  if (pointCount === 0 || input.retainedComponentIds.size === 0 || input.quarantinedComponentIds.size === 0) {
    return {
      rescanComponentIds: [],
      weakClusterCount: 0,
      bridgeClusterCount: 0,
      examinedWeakPointCount: 0,
      neighbourChecks: 0,
      truncated: false,
    };
  }

  const voxel = clamp(Number.isFinite(input.voxelSizeMeters) ? Math.abs(input.voxelSizeMeters) : 0.04, 0.025, 0.10);
  const linkRadius = clamp(voxel * 1.90, 0.050, 0.13);
  const cellSize = linkRadius;
  const linkRadiusSquared = linkRadius * linkRadius;
  const buckets = new Map<string, number[]>();
  const weakIndices: number[] = [];

  for (let index = 0; index < pointCount; index += 1) {
    const offset = index * 3;
    const x = Number(input.positions[offset]);
    const y = Number(input.positions[offset + 1]);
    const z = Number(input.positions[offset + 2]);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    const gx = Math.floor(x / cellSize);
    const gy = Math.floor(y / cellSize);
    const gz = Math.floor(z / cellSize);
    const key = cellKey(gx, gy, gz);
    const bucket = buckets.get(key);
    if (bucket === undefined) buckets.set(key, [index]);
    else bucket.push(index);
    if (Number(input.componentIdByPoint[index]) < 0 && weakIndices.length < MAX_WEAK_POINTS) weakIndices.push(index);
  }

  const totalWeakPoints = Array.from({ length: pointCount }, (_, index) => Number(input.componentIdByPoint[index]) < 0 ? 1 : 0)
    .reduce((sum, value) => sum + value, 0);
  let truncated = totalWeakPoints > weakIndices.length;
  const visited = new Uint8Array(pointCount);
  const rescan = new Set<number>();
  let weakClusterCount = 0;
  let bridgeClusterCount = 0;
  let neighbourChecks = 0;

  for (const seed of weakIndices) {
    if (visited[seed] !== 0) continue;
    visited[seed] = 1;
    const queue = [seed];
    const touchedRetained = new Set<number>();
    const touchedQuarantined = new Set<number>();

    for (let read = 0; read < queue.length; read += 1) {
      const current = queue[read];
      const offset = current * 3;
      const x = Number(input.positions[offset]);
      const y = Number(input.positions[offset + 1]);
      const z = Number(input.positions[offset + 2]);
      const gx = Math.floor(x / cellSize);
      const gy = Math.floor(y / cellSize);
      const gz = Math.floor(z / cellSize);

      for (let dx = -1; dx <= 1; dx += 1) {
        for (let dy = -1; dy <= 1; dy += 1) {
          for (let dz = -1; dz <= 1; dz += 1) {
            const bucket = buckets.get(cellKey(gx + dx, gy + dy, gz + dz));
            if (bucket === undefined) continue;
            for (const candidate of bucket) {
              if (candidate === current) continue;
              neighbourChecks += 1;
              if (neighbourChecks > MAX_NEIGHBOUR_CHECKS) {
                truncated = true;
                break;
              }
              const candidateOffset = candidate * 3;
              const px = Number(input.positions[candidateOffset]);
              const py = Number(input.positions[candidateOffset + 1]);
              const pz = Number(input.positions[candidateOffset + 2]);
              const ddx = x - px;
              const ddy = y - py;
              const ddz = z - pz;
              if (ddx * ddx + ddy * ddy + ddz * ddz > linkRadiusSquared) continue;
              const component = Number(input.componentIdByPoint[candidate]);
              if (component < 0) {
                if (visited[candidate] === 0 && queue.length < MAX_WEAK_POINTS) {
                  visited[candidate] = 1;
                  queue.push(candidate);
                }
              } else if (input.retainedComponentIds.has(component)) {
                touchedRetained.add(component);
              } else if (input.quarantinedComponentIds.has(component)) {
                touchedQuarantined.add(component);
              }
            }
            if (neighbourChecks > MAX_NEIGHBOUR_CHECKS) break;
          }
          if (neighbourChecks > MAX_NEIGHBOUR_CHECKS) break;
        }
        if (neighbourChecks > MAX_NEIGHBOUR_CHECKS) break;
      }
      if (neighbourChecks > MAX_NEIGHBOUR_CHECKS) break;
    }

    weakClusterCount += 1;
    if (
      queue.length >= MIN_WEAK_BRIDGE_POINTS &&
      touchedRetained.size > 0 &&
      touchedQuarantined.size > 0
    ) {
      bridgeClusterCount += 1;
      for (const component of touchedQuarantined) rescan.add(component);
    }
    if (neighbourChecks > MAX_NEIGHBOUR_CHECKS) break;
  }

  return {
    rescanComponentIds: [...rescan].sort((a, b) => a - b),
    weakClusterCount,
    bridgeClusterCount,
    examinedWeakPointCount: weakIndices.length,
    neighbourChecks,
    truncated,
  };
}
