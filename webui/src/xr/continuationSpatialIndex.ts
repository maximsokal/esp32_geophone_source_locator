export interface ContinuationSpatialPoint {
  x: number;
  y: number;
  z: number;
  sourceIndex: number;
}

export type ContinuationSpatialHash<T extends ContinuationSpatialPoint = ContinuationSpatialPoint> = ReadonlyMap<string, T[]>;

export interface ContinuationSpatialSearchOperations {
  bucketLookups: number;
  candidateDistanceChecks: number;
}

function cellCoordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

export function buildContinuationSpatialHash<T extends ContinuationSpatialPoint>(
  points: readonly T[],
  cellSizeMeters: number,
): Map<string, T[]> {
  const map = new Map<string, T[]>();
  for (const point of points) {
    const key = `${cellCoordinate(point.x, cellSizeMeters)}:${cellCoordinate(point.y, cellSizeMeters)}:${cellCoordinate(point.z, cellSizeMeters)}`;
    const bucket = map.get(key);
    if (bucket === undefined) map.set(key, [point]);
    else bucket.push(point);
  }
  return map;
}

export function nearestContinuationPoint<T extends ContinuationSpatialPoint>(
  transformed: ContinuationSpatialPoint,
  hash: ContinuationSpatialHash<T>,
  cellSizeMeters: number,
  maximumDistanceMeters: number,
  operations?: ContinuationSpatialSearchOperations,
  acceptCandidate?: (candidate: T) => boolean,
): { point: T; distanceSquared: number } | null {
  const gx = cellCoordinate(transformed.x, cellSizeMeters);
  const gy = cellCoordinate(transformed.y, cellSizeMeters);
  const gz = cellCoordinate(transformed.z, cellSizeMeters);
  const maxDistanceSquared = maximumDistanceMeters * maximumDistanceMeters;
  const cellRadius = Math.ceil(maximumDistanceMeters / cellSizeMeters);
  let best: T | null = null;
  let bestDistanceSquared = maxDistanceSquared;
  for (let dz = -cellRadius; dz <= cellRadius; dz += 1) {
    for (let dy = -cellRadius; dy <= cellRadius; dy += 1) {
      for (let dx = -cellRadius; dx <= cellRadius; dx += 1) {
        if (operations !== undefined) operations.bucketLookups += 1;
        const bucket = hash.get(`${gx + dx}:${gy + dy}:${gz + dz}`);
        if (bucket === undefined) continue;
        for (const candidate of bucket) {
          if (operations !== undefined) operations.candidateDistanceChecks += 1;
          if (acceptCandidate !== undefined && !acceptCandidate(candidate)) continue;
          const ddx = candidate.x - transformed.x;
          const ddy = candidate.y - transformed.y;
          const ddz = candidate.z - transformed.z;
          const distanceSquared = ddx * ddx + ddy * ddy + ddz * ddz;
          if (distanceSquared <= bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            best = candidate;
          }
        }
      }
    }
  }
  return best === null ? null : { point: best, distanceSquared: bestDistanceSquared };
}
