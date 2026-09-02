export type FreeSpacePoint = readonly [number, number, number];

export interface VerifiedFreeSpaceSnapshot {
  freeSpacePositions?: ArrayLike<number>;
  freeSpaceVoxelSizeMeters?: number;
}

function coordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

function keyFromCoordinates(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function key(point: FreeSpacePoint, size: number): string {
  return keyFromCoordinates(
    coordinate(point[0], size),
    coordinate(point[1], size),
    coordinate(point[2], size),
  );
}

/**
 * Read-only hash of ray-verified FREE voxels.
 *
 * This is deliberately exact-cell evidence. A nearby FREE voxel must not expand
 * into UNKNOWN or override physical surface evidence merely because it is close.
 * Callers may sample multiple metric positions explicitly when checking a line or
 * ray; this class never invents an implicit dilation radius.
 */
export class VerifiedFreeSpaceIndex {
  readonly voxelSizeMeters: number;
  private readonly occupiedKeys: Set<string>;

  private constructor(voxelSizeMeters: number, occupiedKeys: Set<string>) {
    this.voxelSizeMeters = voxelSizeMeters;
    this.occupiedKeys = occupiedKeys;
  }

  static from(snapshot: VerifiedFreeSpaceSnapshot): VerifiedFreeSpaceIndex | null {
    const positions = snapshot.freeSpacePositions;
    const size = Number(snapshot.freeSpaceVoxelSizeMeters);
    if (
      positions === undefined ||
      !Number.isFinite(size) ||
      size <= 0 ||
      positions.length < 3
    ) return null;

    const keys = new Set<string>();
    for (let offset = 0; offset + 2 < positions.length; offset += 3) {
      const x = Number(positions[offset]);
      const y = Number(positions[offset + 1]);
      const z = Number(positions[offset + 2]);
      if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
      keys.add(key([x, y, z], size));
    }
    return keys.size > 0 ? new VerifiedFreeSpaceIndex(size, keys) : null;
  }

  contains(point: FreeSpacePoint): boolean {
    return this.occupiedKeys.has(key(point, this.voxelSizeMeters));
  }

  /** Number of explicitly sampled positions that land in verified FREE voxels. */
  count(samples: readonly FreeSpacePoint[]): number {
    let hits = 0;
    for (const sample of samples) if (this.contains(sample)) hits += 1;
    return hits;
  }
}
