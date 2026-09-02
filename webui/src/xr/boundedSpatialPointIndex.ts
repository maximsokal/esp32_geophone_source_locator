export type SpatialPoint = readonly [number, number, number];

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function coordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

function key(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

/**
 * Small deterministic voxel hash for repeated local-neighbour queries.
 *
 * Structural corner/endpoint solvers only consume a 10-18 cm neighbourhood,
 * so rescanning the complete surfel cloud for every seed is wasted O(S*N) work.
 * This index is built once in O(N) memory/time and visits only occupied buckets
 * intersecting the requested radius. Exact squared-distance filtering keeps the
 * hash from changing solver semantics at cell boundaries.
 */
export class BoundedSpatialPointIndex {
  private readonly buckets = new Map<string, number[]>();
  readonly pointCount: number;
  readonly cellSizeMeters: number;

  constructor(
    private readonly positions: ArrayLike<number>,
    pointCount: number,
    cellSizeMeters: number,
  ) {
    this.pointCount = Math.min(Math.max(0, Math.floor(pointCount)), Math.floor(positions.length / 3));
    this.cellSizeMeters = clamp(
      Number.isFinite(cellSizeMeters) ? cellSizeMeters : 0.09,
      0.04,
      0.20,
    );
    for (let index = 0; index < this.pointCount; index += 1) {
      const offset = index * 3;
      const x = Number(positions[offset]);
      const y = Number(positions[offset + 1]);
      const z = Number(positions[offset + 2]);
      if (![x, y, z].every(Number.isFinite)) continue;
      const bucketKey = key(
        coordinate(x, this.cellSizeMeters),
        coordinate(y, this.cellSizeMeters),
        coordinate(z, this.cellSizeMeters),
      );
      const bucket = this.buckets.get(bucketKey) ?? [];
      bucket.push(index);
      this.buckets.set(bucketKey, bucket);
    }
  }

  queryRadius(center: SpatialPoint, radiusMeters: number): readonly number[] {
    const radius = Math.max(0, Number.isFinite(radiusMeters) ? radiusMeters : 0);
    if (radius <= 0 || !center.every(Number.isFinite)) return [];
    const radiusSquared = radius * radius;
    const gx = coordinate(center[0], this.cellSizeMeters);
    const gy = coordinate(center[1], this.cellSizeMeters);
    const gz = coordinate(center[2], this.cellSizeMeters);
    const reach = Math.max(1, Math.ceil(radius / this.cellSizeMeters));
    const found: Array<{ index: number; squared: number }> = [];

    for (let dx = -reach; dx <= reach; dx += 1) {
      for (let dy = -reach; dy <= reach; dy += 1) {
        for (let dz = -reach; dz <= reach; dz += 1) {
          const bucket = this.buckets.get(key(gx + dx, gy + dy, gz + dz));
          if (bucket === undefined) continue;
          for (const index of bucket) {
            const offset = index * 3;
            const px = Number(this.positions[offset]);
            const py = Number(this.positions[offset + 1]);
            const pz = Number(this.positions[offset + 2]);
            const x = px - center[0];
            const y = py - center[1];
            const z = pz - center[2];
            const squared = x * x + y * y + z * z;
            if (squared <= radiusSquared) found.push({ index, squared });
          }
        }
      }
    }

    // Distance order matches the local solvers' natural preference and makes
    // bounded downstream truncation deterministic. Source index breaks ties.
    found.sort((left, right) => left.squared - right.squared || left.index - right.index);
    return found.map((item) => item.index);
  }
}
