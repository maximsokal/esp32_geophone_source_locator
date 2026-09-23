import type { ScanPointCloudSnapshot } from './webxrRuntime';

export interface ScanContinuationDiagnostics {
  yawRadians: number;
  translation: readonly [number, number, number];
  inliers: number;
  overlapRatio: number;
  rmsMeters: number;
  baseSampleCount: number;
  extensionSampleCount: number;
}

export type ScanContinuationResult =
  | { ok: true; snapshot: ScanPointCloudSnapshot; diagnostics: ScanContinuationDiagnostics }
  | { ok: false; reason: string; diagnostics: ScanContinuationDiagnostics | null };

interface RigidTransform {
  yaw: number;
  tx: number;
  ty: number;
  tz: number;
}

interface Point3 {
  x: number;
  y: number;
  z: number;
  sourceIndex: number;
}

interface Match {
  source: Point3;
  target: Point3;
  distanceSquared: number;
}

const SAMPLE_LIMIT = 900;
const YAW_STEP_RADIANS = Math.PI / 12; // 15 degrees.
const REGISTRATION_CELL_METERS = 0.12;
const REGISTRATION_MAX_DISTANCE_METERS = 0.18;
const REGISTRATION_MIN_INLIERS = 90;
const REGISTRATION_MIN_OVERLAP = 0.18;
const REGISTRATION_MAX_RMS_METERS = 0.11;
const REFINEMENT_ITERATIONS = 6;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function pointCount(values: ArrayLike<number>): number {
  return Math.floor(values.length / 3);
}

function samplePositions(values: Float32Array, limit = SAMPLE_LIMIT): Point3[] {
  const count = pointCount(values);
  if (count === 0) return [];
  const take = Math.min(count, limit);
  const result: Point3[] = [];
  for (let sample = 0; sample < take; sample += 1) {
    const index = Math.min(count - 1, Math.floor((sample * count) / take));
    const offset = index * 3;
    const x = Number(values[offset]);
    const y = Number(values[offset + 1]);
    const z = Number(values[offset + 2]);
    if (finite(x) && finite(y) && finite(z)) result.push({ x, y, z, sourceIndex: index });
  }
  return result;
}

function centroid(points: readonly Point3[]): readonly [number, number, number] {
  let x = 0, y = 0, z = 0;
  for (const point of points) {
    x += point.x;
    y += point.y;
    z += point.z;
  }
  const divisor = Math.max(1, points.length);
  return [x / divisor, y / divisor, z / divisor];
}

function transformPoint(point: Point3, transform: RigidTransform): Point3 {
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  return {
    x: c * point.x - s * point.y + transform.tx,
    y: s * point.x + c * point.y + transform.ty,
    z: point.z + transform.tz,
    sourceIndex: point.sourceIndex,
  };
}

function voxelCoordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

function voxelKey(x: number, y: number, z: number, size: number): string {
  return `${voxelCoordinate(x, size)}:${voxelCoordinate(y, size)}:${voxelCoordinate(z, size)}`;
}

function buildPointHash(points: readonly Point3[], cellSize: number): Map<string, Point3[]> {
  const map = new Map<string, Point3[]>();
  for (const point of points) {
    const key = voxelKey(point.x, point.y, point.z, cellSize);
    const bucket = map.get(key);
    if (bucket === undefined) map.set(key, [point]);
    else bucket.push(point);
  }
  return map;
}

function nearestPoint(
  transformed: Point3,
  hash: ReadonlyMap<string, Point3[]>,
  cellSize: number,
  maximumDistanceMeters: number,
): { point: Point3; distanceSquared: number } | null {
  const gx = voxelCoordinate(transformed.x, cellSize);
  const gy = voxelCoordinate(transformed.y, cellSize);
  const gz = voxelCoordinate(transformed.z, cellSize);
  const maxDistanceSquared = maximumDistanceMeters * maximumDistanceMeters;
  let best: Point3 | null = null;
  let bestDistanceSquared = maxDistanceSquared;
  for (let dz = -1; dz <= 1; dz += 1) {
    for (let dy = -1; dy <= 1; dy += 1) {
      for (let dx = -1; dx <= 1; dx += 1) {
        const bucket = hash.get(`${gx + dx}:${gy + dy}:${gz + dz}`);
        if (bucket === undefined) continue;
        for (const candidate of bucket) {
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

function collectMatches(
  source: readonly Point3[],
  targetHash: ReadonlyMap<string, Point3[]>,
  transform: RigidTransform,
): Match[] {
  /*
   * Keep at most one source correspondence per saved target point. Without this
   * constraint a compressed/distorted continuation can collapse many extension
   * samples onto the same few base samples and fake a high inlier/overlap score.
   * The smallest-residual source wins deterministically for each target.
   */
  const bestByTarget = new Map<number, Match>();
  for (const sourcePoint of source) {
    const transformed = transformPoint(sourcePoint, transform);
    const nearest = nearestPoint(
      transformed,
      targetHash,
      REGISTRATION_CELL_METERS,
      REGISTRATION_MAX_DISTANCE_METERS,
    );
    if (nearest === null) continue;
    const match: Match = {
      source: sourcePoint,
      target: nearest.point,
      distanceSquared: nearest.distanceSquared,
    };
    const previous = bestByTarget.get(nearest.point.sourceIndex);
    if (previous === undefined || match.distanceSquared < previous.distanceSquared) {
      bestByTarget.set(nearest.point.sourceIndex, match);
    }
  }
  return [...bestByTarget.values()];
}

function solveYawTranslation(matches: readonly Match[]): RigidTransform | null {
  if (matches.length < 3) return null;
  let sx = 0, sy = 0, sz = 0, tx = 0, ty = 0, tz = 0;
  for (const match of matches) {
    sx += match.source.x; sy += match.source.y; sz += match.source.z;
    tx += match.target.x; ty += match.target.y; tz += match.target.z;
  }
  const inv = 1 / matches.length;
  sx *= inv; sy *= inv; sz *= inv; tx *= inv; ty *= inv; tz *= inv;
  let a = 0, b = 0;
  for (const match of matches) {
    const xs = match.source.x - sx;
    const ys = match.source.y - sy;
    const xt = match.target.x - tx;
    const yt = match.target.y - ty;
    a += xs * xt + ys * yt;
    b += xs * yt - ys * xt;
  }
  const yaw = Math.atan2(b, a);
  const c = Math.cos(yaw);
  const s = Math.sin(yaw);
  return {
    yaw,
    tx: tx - (c * sx - s * sy),
    ty: ty - (s * sx + c * sy),
    tz: tz - sz,
  };
}

function scoreTransform(
  source: readonly Point3[],
  targetHash: ReadonlyMap<string, Point3[]>,
  transform: RigidTransform,
  denominator: number,
): { matches: Match[]; rmsMeters: number; overlapRatio: number } {
  const matches = collectMatches(source, targetHash, transform);
  const squared = matches.reduce((sum, match) => sum + match.distanceSquared, 0);
  return {
    matches,
    rmsMeters: matches.length > 0 ? Math.sqrt(squared / matches.length) : Number.POSITIVE_INFINITY,
    overlapRatio: matches.length / Math.max(1, denominator),
  };
}

function registerExtension(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
): { transform: RigidTransform; diagnostics: ScanContinuationDiagnostics } | null {
  const basePoints = samplePositions(base.positions);
  const extensionPoints = samplePositions(extension.positions);
  if (basePoints.length < 120 || extensionPoints.length < 120) return null;
  const targetHash = buildPointHash(basePoints, REGISTRATION_CELL_METERS);
  const baseCentroid = centroid(basePoints);
  const extensionCentroid = centroid(extensionPoints);
  const denominator = Math.min(basePoints.length, extensionPoints.length);
  let best: { transform: RigidTransform; matches: Match[]; rmsMeters: number; overlapRatio: number } | null = null;

  for (let hypothesis = 0; hypothesis < Math.round((Math.PI * 2) / YAW_STEP_RADIANS); hypothesis += 1) {
    const yaw = hypothesis * YAW_STEP_RADIANS;
    const c = Math.cos(yaw);
    const s = Math.sin(yaw);
    let transform: RigidTransform = {
      yaw,
      tx: baseCentroid[0] - (c * extensionCentroid[0] - s * extensionCentroid[1]),
      ty: baseCentroid[1] - (s * extensionCentroid[0] + c * extensionCentroid[1]),
      tz: baseCentroid[2] - extensionCentroid[2],
    };

    for (let iteration = 0; iteration < REFINEMENT_ITERATIONS; iteration += 1) {
      const matches = collectMatches(extensionPoints, targetHash, transform);
      const solved = solveYawTranslation(matches);
      if (solved === null) break;
      transform = solved;
    }

    const score = scoreTransform(extensionPoints, targetHash, transform, denominator);
    if (
      best === null ||
      score.matches.length > best.matches.length ||
      (score.matches.length === best.matches.length && score.rmsMeters < best.rmsMeters)
    ) {
      best = { transform, ...score };
    }
  }

  if (best === null) return null;
  const diagnostics: ScanContinuationDiagnostics = {
    yawRadians: best.transform.yaw,
    translation: [best.transform.tx, best.transform.ty, best.transform.tz],
    inliers: best.matches.length,
    overlapRatio: best.overlapRatio,
    rmsMeters: best.rmsMeters,
    baseSampleCount: basePoints.length,
    extensionSampleCount: extensionPoints.length,
  };
  if (
    diagnostics.inliers < REGISTRATION_MIN_INLIERS ||
    diagnostics.overlapRatio < REGISTRATION_MIN_OVERLAP ||
    diagnostics.rmsMeters > REGISTRATION_MAX_RMS_METERS
  ) return { transform: best.transform, diagnostics };
  return { transform: best.transform, diagnostics };
}

function transformPositions(values: Float32Array, transform: RigidTransform): Float32Array {
  const result = new Float32Array(values.length);
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  for (let offset = 0; offset + 2 < values.length; offset += 3) {
    const x = values[offset];
    const y = values[offset + 1];
    result[offset] = c * x - s * y + transform.tx;
    result[offset + 1] = s * x + c * y + transform.ty;
    result[offset + 2] = values[offset + 2] + transform.tz;
  }
  return result;
}

function transformNormals(values: Float32Array, transform: RigidTransform): Float32Array {
  const result = new Float32Array(values.length);
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  for (let offset = 0; offset + 2 < values.length; offset += 3) {
    const x = values[offset];
    const y = values[offset + 1];
    result[offset] = c * x - s * y;
    result[offset + 1] = s * x + c * y;
    result[offset + 2] = values[offset + 2];
  }
  return result;
}

function appendFloatArrays(a: Float32Array, b: Float32Array): Float32Array {
  const result = new Float32Array(a.length + b.length);
  result.set(a, 0);
  result.set(b, a.length);
  return result;
}

function mergeSurface(
  basePositions: Float32Array,
  baseConfidence: Float32Array,
  baseNormals: Float32Array,
  extensionPositions: Float32Array,
  extensionConfidence: Float32Array,
  extensionNormals: Float32Array,
  voxelSizeMeters: number,
): { positions: Float32Array; confidence: Float32Array; normals: Float32Array } {
  const keys = new Set<string>();
  const positions: number[] = [];
  const confidence: number[] = [];
  const normals: number[] = [];
  const add = (p: Float32Array, c: Float32Array, n: Float32Array, index: number): void => {
    const offset = index * 3;
    const key = voxelKey(p[offset], p[offset + 1], p[offset + 2], voxelSizeMeters);
    if (keys.has(key)) return;
    keys.add(key);
    positions.push(p[offset], p[offset + 1], p[offset + 2]);
    confidence.push(c[index] ?? 0);
    normals.push(n[offset] ?? 0, n[offset + 1] ?? 0, n[offset + 2] ?? 1);
  };
  for (let index = 0; index < pointCount(basePositions); index += 1) add(basePositions, baseConfidence, baseNormals, index);
  for (let index = 0; index < pointCount(extensionPositions); index += 1) add(extensionPositions, extensionConfidence, extensionNormals, index);
  return {
    positions: new Float32Array(positions),
    confidence: new Float32Array(confidence),
    normals: new Float32Array(normals),
  };
}

function mergeSimplePoints(
  basePositions: Float32Array,
  baseConfidence: Float32Array,
  extensionPositions: Float32Array,
  extensionConfidence: Float32Array,
  stablePositions: Float32Array,
  voxelSizeMeters: number,
): { positions: Float32Array; confidence: Float32Array } {
  const blocked = new Set<string>();
  for (let index = 0; index < pointCount(stablePositions); index += 1) {
    const offset = index * 3;
    blocked.add(voxelKey(stablePositions[offset], stablePositions[offset + 1], stablePositions[offset + 2], voxelSizeMeters));
  }
  const keys = new Set<string>();
  const positions: number[] = [];
  const confidence: number[] = [];
  const add = (p: Float32Array, c: Float32Array, index: number): void => {
    const offset = index * 3;
    const key = voxelKey(p[offset], p[offset + 1], p[offset + 2], voxelSizeMeters);
    if (blocked.has(key) || keys.has(key)) return;
    keys.add(key);
    positions.push(p[offset], p[offset + 1], p[offset + 2]);
    confidence.push(c[index] ?? 0);
  };
  for (let index = 0; index < pointCount(basePositions); index += 1) add(basePositions, baseConfidence, index);
  for (let index = 0; index < pointCount(extensionPositions); index += 1) add(extensionPositions, extensionConfidence, index);
  return { positions: new Float32Array(positions), confidence: new Float32Array(confidence) };
}

function assertCompleteOccupancy(snapshot: ScanPointCloudSnapshot): string | null {
  if (pointCount(snapshot.freeAirPositions) !== snapshot.knownFreeVoxelCount) return 'saved FREE evidence is display-truncated';
  if (pointCount(snapshot.occupiedSurfacePositions) !== snapshot.occupiedSurfaceVoxelCount) return 'saved OCCUPIED evidence is display-truncated';
  if (pointCount(snapshot.unknownCandidatePositions) !== snapshot.unknownCandidateVoxelCount) return 'saved UNKNOWN evidence is display-truncated';
  return null;
}

function positionsMap(values: Float32Array, voxelSize: number): Map<string, readonly [number, number, number]> {
  const result = new Map<string, readonly [number, number, number]>();
  for (let index = 0; index < pointCount(values); index += 1) {
    const offset = index * 3;
    const point: readonly [number, number, number] = [values[offset], values[offset + 1], values[offset + 2]];
    result.set(voxelKey(point[0], point[1], point[2], voxelSize), point);
  }
  return result;
}

function mapToPositions(map: ReadonlyMap<string, readonly [number, number, number]>): Float32Array {
  const result = new Float32Array(map.size * 3);
  let offset = 0;
  for (const point of map.values()) {
    result[offset++] = point[0]; result[offset++] = point[1]; result[offset++] = point[2];
  }
  return result;
}

function mergeOccupancy(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  transform: RigidTransform,
): {
  freeAirPositions: Float32Array;
  occupiedSurfacePositions: Float32Array;
  unknownCandidatePositions: Float32Array;
} | null {
  if (assertCompleteOccupancy(base) !== null || assertCompleteOccupancy(extension) !== null) return null;
  if (Math.abs(base.occupancyVoxelSizeMeters - extension.occupancyVoxelSizeMeters) > 1e-6) return null;
  const voxel = base.occupancyVoxelSizeMeters;
  const occupied = positionsMap(base.occupiedSurfacePositions, voxel);
  for (const [key, point] of positionsMap(transformPositions(extension.occupiedSurfacePositions, transform), voxel)) occupied.set(key, point);
  const free = positionsMap(base.freeAirPositions, voxel);
  for (const [key, point] of positionsMap(transformPositions(extension.freeAirPositions, transform), voxel)) {
    if (!occupied.has(key)) free.set(key, point);
  }
  for (const key of occupied.keys()) free.delete(key);
  const unknown = positionsMap(base.unknownCandidatePositions, voxel);
  for (const [key, point] of positionsMap(transformPositions(extension.unknownCandidatePositions, transform), voxel)) {
    if (!occupied.has(key) && !free.has(key)) unknown.set(key, point);
  }
  for (const key of occupied.keys()) unknown.delete(key);
  for (const key of free.keys()) unknown.delete(key);
  return {
    freeAirPositions: mapToPositions(free),
    occupiedSurfacePositions: mapToPositions(occupied),
    unknownCandidatePositions: mapToPositions(unknown),
  };
}

function mergeTsdf(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  transform: RigidTransform,
): Pick<ScanPointCloudSnapshot,
  'tsdfPositions' | 'tsdfNormals' | 'tsdfConfidence' | 'tsdfResidualMeters' |
  'tsdfNormalConfidence' | 'tsdfCoverageConfidence' | 'tsdfCoverageReady' | 'tsdfPointCount'> {
  const extensionPositions = transformPositions(extension.tsdfPositions, transform);
  const extensionNormals = transformNormals(extension.tsdfNormals, transform);
  const voxel = Math.max(0.02, Math.min(base.voxelSizeMeters, extension.voxelSizeMeters));
  const keys = new Set<string>();
  const positions: number[] = [], normals: number[] = [], confidence: number[] = [], residual: number[] = [];
  const normalConfidence: number[] = [], coverageConfidence: number[] = [], coverageReady: number[] = [];
  const add = (
    source: ScanPointCloudSnapshot,
    p: Float32Array,
    n: Float32Array,
    index: number,
  ): void => {
    const offset = index * 3;
    const key = voxelKey(p[offset], p[offset + 1], p[offset + 2], voxel);
    if (keys.has(key)) return;
    keys.add(key);
    positions.push(p[offset], p[offset + 1], p[offset + 2]);
    normals.push(n[offset], n[offset + 1], n[offset + 2]);
    confidence.push(source.tsdfConfidence[index] ?? 0);
    residual.push(source.tsdfResidualMeters[index] ?? 0);
    normalConfidence.push(source.tsdfNormalConfidence[index] ?? 0);
    coverageConfidence.push(source.tsdfCoverageConfidence[index] ?? 0);
    coverageReady.push(source.tsdfCoverageReady[index] ?? 0);
  };
  for (let index = 0; index < base.tsdfPointCount; index += 1) add(base, base.tsdfPositions, base.tsdfNormals, index);
  for (let index = 0; index < extension.tsdfPointCount; index += 1) add(extension, extensionPositions, extensionNormals, index);
  return {
    tsdfPositions: new Float32Array(positions),
    tsdfNormals: new Float32Array(normals),
    tsdfConfidence: new Float32Array(confidence),
    tsdfResidualMeters: new Float32Array(residual),
    tsdfNormalConfidence: new Float32Array(normalConfidence),
    tsdfCoverageConfidence: new Float32Array(coverageConfidence),
    tsdfCoverageReady: new Uint8Array(coverageReady),
    tsdfPointCount: confidence.length,
  };
}

function boundsFor(values: Float32Array): ScanPointCloudSnapshot['bounds'] {
  if (values.length < 3) return null;
  let minX = Number.POSITIVE_INFINITY, minY = Number.POSITIVE_INFINITY, minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY, maxY = Number.NEGATIVE_INFINITY, maxZ = Number.NEGATIVE_INFINITY;
  for (let offset = 0; offset + 2 < values.length; offset += 3) {
    minX = Math.min(minX, values[offset]); minY = Math.min(minY, values[offset + 1]); minZ = Math.min(minZ, values[offset + 2]);
    maxX = Math.max(maxX, values[offset]); maxY = Math.max(maxY, values[offset + 1]); maxZ = Math.max(maxZ, values[offset + 2]);
  }
  return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

function transformedManualPoints(snapshot: ScanPointCloudSnapshot, transform: RigidTransform, startId: number) {
  return snapshot.manualControlPoints.map((point, index) => {
    const transformed = transformPoint({ x: point.position[0], y: point.position[1], z: point.position[2], sourceIndex: 0 }, transform);
    return { id: startId + index, position: [transformed.x, transformed.y, transformed.z] as const };
  });
}

function transformedEdges(snapshot: ScanPointCloudSnapshot, transform: RigidTransform, startId: number) {
  return snapshot.operatorEdges.map((edge, index) => {
    const a = transformPoint({ x: edge.start[0], y: edge.start[1], z: edge.start[2], sourceIndex: 0 }, transform);
    const b = transformPoint({ x: edge.end[0], y: edge.end[1], z: edge.end[2], sourceIndex: 0 }, transform);
    return {
      id: startId + index,
      start: [a.x, a.y, a.z] as const,
      end: [b.x, b.y, b.z] as const,
      confidence: edge.confidence,
    };
  });
}

function weightedAverage(a: number, aCount: number, b: number, bCount: number): number {
  const total = aCount + bCount;
  return total > 0 ? (a * aCount + b * bCount) / total : 0;
}

export function registerAndMergeScanContinuation(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
): ScanContinuationResult {
  if (base.pointCount < 120 || extension.pointCount < 120) {
    return { ok: false, reason: 'Need at least 120 stable points in both the saved scan and the continuation.', diagnostics: null };
  }
  const occupancyProblem = assertCompleteOccupancy(base) ?? assertCompleteOccupancy(extension);
  if (occupancyProblem !== null) {
    return { ok: false, reason: `Cannot safely continue: ${occupancyProblem}. Old saved scan was kept.`, diagnostics: null };
  }
  if (Math.abs(base.occupancyVoxelSizeMeters - extension.occupancyVoxelSizeMeters) > 1e-6) {
    return { ok: false, reason: 'Cannot safely continue scans with different occupancy voxel sizes.', diagnostics: null };
  }

  const registration = registerExtension(base, extension);
  if (registration === null) {
    return { ok: false, reason: 'Could not align the continuation. Start by scanning an overlapping part of the saved scene.', diagnostics: null };
  }
  const diagnostics = registration.diagnostics;
  if (
    diagnostics.inliers < REGISTRATION_MIN_INLIERS ||
    diagnostics.overlapRatio < REGISTRATION_MIN_OVERLAP ||
    diagnostics.rmsMeters > REGISTRATION_MAX_RMS_METERS
  ) {
    return {
      ok: false,
      reason: `Continuation alignment rejected: ${diagnostics.inliers} matches, ${Math.round(diagnostics.overlapRatio * 100)}% overlap, ${(diagnostics.rmsMeters * 100).toFixed(1)} cm RMS. Scan more overlap; old saved scan was kept.`,
      diagnostics,
    };
  }

  const transform = registration.transform;
  const extensionStablePositions = transformPositions(extension.positions, transform);
  const extensionStableNormals = transformNormals(extension.normals, transform);
  const mergeVoxel = Math.max(0.02, Math.min(base.voxelSizeMeters, extension.voxelSizeMeters));
  const stable = mergeSurface(
    base.positions,
    base.confidence,
    base.normals,
    extensionStablePositions,
    extension.confidence,
    extensionStableNormals,
    mergeVoxel,
  );
  const tentative = mergeSimplePoints(
    base.tentativePositions,
    base.tentativeConfidence,
    transformPositions(extension.tentativePositions, transform),
    extension.tentativeConfidence,
    stable.positions,
    mergeVoxel,
  );
  const occupancy = mergeOccupancy(base, extension, transform);
  if (occupancy === null) {
    return { ok: false, reason: 'Continuation occupancy evidence could not be merged safely. Old saved scan was kept.', diagnostics };
  }
  const tsdf = mergeTsdf(base, extension, transform);
  const maxManualId = base.manualControlPoints.reduce((max, point) => Math.max(max, point.id), 0);
  const maxEdgeId = base.operatorEdges.reduce((max, edge) => Math.max(max, edge.id), 0);
  const manualControlPoints = [
    ...base.manualControlPoints,
    ...transformedManualPoints(extension, transform, maxManualId + 1),
  ];
  const operatorEdges = [
    ...base.operatorEdges,
    ...transformedEdges(extension, transform, maxEdgeId + 1),
  ];
  const fullPositions = appendFloatArrays(stable.positions, tentative.positions);

  const snapshot: ScanPointCloudSnapshot = {
    ...base,
    positions: stable.positions,
    confidence: stable.confidence,
    normals: stable.normals,
    pointCount: pointCount(stable.positions),
    tentativePositions: tentative.positions,
    tentativeConfidence: tentative.confidence,
    tentativePointCount: pointCount(tentative.positions),
    rawPointCount: base.rawPointCount + extension.rawPointCount,
    bounds: boundsFor(stable.positions),
    fullBounds: boundsFor(fullPositions),
    depthFrameCount: base.depthFrameCount + extension.depthFrameCount,
    sampledDepthFrames: base.sampledDepthFrames + extension.sampledDepthFrames,
    capacityReached: base.capacityReached || extension.capacityReached,
    candidateSampleCount: base.candidateSampleCount + extension.candidateSampleCount,
    discardedSampleCount: base.discardedSampleCount + extension.discardedSampleCount,
    replacementCount: base.replacementCount + extension.replacementCount,
    rebalanceEvictionCount: base.rebalanceEvictionCount + extension.rebalanceEvictionCount,
    coverageRejectedCount: base.coverageRejectedCount + extension.coverageRejectedCount,
    promotedPointCount: pointCount(stable.positions),
    integrationKeyframes: base.integrationKeyframes + extension.integrationKeyframes,
    polishFrames: base.polishFrames + extension.polishFrames,
    residualRejectedSamples: base.residualRejectedSamples + extension.residualRejectedSamples,
    staleTentativePruned: base.staleTentativePruned + extension.staleTentativePruned,
    averageStableResidualMeters: weightedAverage(
      base.averageStableResidualMeters,
      base.pointCount,
      extension.averageStableResidualMeters,
      extension.pointCount,
    ),
    coverageCellCount: base.coverageCellCount + extension.coverageCellCount,
    readyCoverageCellCount: base.readyCoverageCellCount + extension.readyCoverageCellCount,
    freeAirPositions: occupancy.freeAirPositions,
    occupiedSurfacePositions: occupancy.occupiedSurfacePositions,
    unknownCandidatePositions: occupancy.unknownCandidatePositions,
    knownFreeVoxelCount: pointCount(occupancy.freeAirPositions),
    occupiedSurfaceVoxelCount: pointCount(occupancy.occupiedSurfacePositions),
    unknownCandidateVoxelCount: pointCount(occupancy.unknownCandidatePositions),
    occupancyEnvelopeBounds: boundsFor(occupancy.unknownCandidatePositions),
    occupancyEnvelopeComponentCount: base.occupancyEnvelopeComponentCount + extension.occupancyEnvelopeComponentCount,
    occupancyQuarantinedSurfaceComponentCount: base.occupancyQuarantinedSurfaceComponentCount + extension.occupancyQuarantinedSurfaceComponentCount,
    occupancyQuarantinedSurfacePointCount: base.occupancyQuarantinedSurfacePointCount + extension.occupancyQuarantinedSurfacePointCount,
    occupancyBridgeRescanComponentCount: base.occupancyBridgeRescanComponentCount + extension.occupancyBridgeRescanComponentCount,
    occupancyBridgeEvidenceTruncated: base.occupancyBridgeEvidenceTruncated || extension.occupancyBridgeEvidenceTruncated,
    occupancyCellsSaturated: Boolean(base.occupancyCellsSaturated) || Boolean(extension.occupancyCellsSaturated),
    occupancyFineVoxelSizeMeters: base.occupancyFineVoxelSizeMeters ?? extension.occupancyFineVoxelSizeMeters,
    occupancyDroppedCellUpdates: (base.occupancyDroppedCellUpdates ?? 0) + (extension.occupancyDroppedCellUpdates ?? 0),
    ...tsdf,
    manualControlPoints,
    operatorEdges,
  };
  return { ok: true, snapshot, diagnostics };
}
