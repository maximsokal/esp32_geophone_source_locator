export type ProjectPoint = readonly [number, number, number];
export type ProjectVector = readonly [number, number, number];

export interface ManualPlacementSurfaceSnapshot {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  pointCount: number;
  voxelSizeMeters: number;
}

export type ManualPlacementMode = 'surface' | 'edge' | 'corner';

export interface ManualPlacementResolution {
  accepted: boolean;
  position: ProjectPoint | null;
  mode: ManualPlacementMode | null;
  confidence: number;
  supportCount: number;
  sourceDistanceMeters: number | null;
  message: string;
}

export type ManualPlacementResolver = (candidate: ProjectPoint) => ManualPlacementResolution;

interface Sample {
  position: ProjectPoint;
  normal: ProjectVector;
  confidence: number;
  distance: number;
}

interface PlaneCluster {
  samples: Sample[];
  normal: ProjectVector;
  center: ProjectPoint;
  weight: number;
}

let activeResolver: ManualPlacementResolver | null = null;

const MIN_SAMPLE_CONFIDENCE = 0.58;
const NORMAL_CLUSTER_DOT = Math.cos((22 * Math.PI) / 180);
const MIN_PLANE_SEPARATION_DOT = Math.cos((32 * Math.PI) / 180);
const MIN_CLUSTER_SAMPLES = 2;
const MAX_LOCAL_SAMPLES = 48;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function subtract(a: ProjectPoint, b: ProjectPoint): ProjectVector {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function add(a: ProjectPoint, b: ProjectVector): ProjectPoint {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

function scale(vector: ProjectVector, amount: number): ProjectVector {
  return [vector[0] * amount, vector[1] * amount, vector[2] * amount];
}

function dot(a: ProjectVector, b: ProjectVector): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function cross(a: ProjectVector, b: ProjectVector): ProjectVector {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function length(vector: ProjectVector): number {
  return Math.hypot(vector[0], vector[1], vector[2]);
}

function normalize(vector: ProjectVector): ProjectVector | null {
  const magnitude = length(vector);
  if (!Number.isFinite(magnitude) || magnitude < 1e-6) return null;
  return [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude];
}

function distance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function weightedPlane(samples: readonly Sample[]): PlaneCluster | null {
  let weight = 0;
  let cx = 0;
  let cy = 0;
  let cz = 0;
  const seed = samples[0]?.normal;
  if (seed === undefined) return null;
  let nx = 0;
  let ny = 0;
  let nz = 0;

  for (const sample of samples) {
    const w = Math.max(0.05, sample.confidence);
    const sign = dot(seed, sample.normal) < 0 ? -1 : 1;
    weight += w;
    cx += sample.position[0] * w;
    cy += sample.position[1] * w;
    cz += sample.position[2] * w;
    nx += sample.normal[0] * w * sign;
    ny += sample.normal[1] * w * sign;
    nz += sample.normal[2] * w * sign;
  }

  if (weight <= 0) return null;
  const normal = normalize([nx, ny, nz]);
  if (normal === null) return null;
  return {
    samples: [...samples],
    normal,
    center: [cx / weight, cy / weight, cz / weight],
    weight,
  };
}

function clusterPlanes(samples: readonly Sample[]): PlaneCluster[] {
  const buckets: Sample[][] = [];
  for (const sample of samples) {
    let bestIndex = -1;
    let bestDot = NORMAL_CLUSTER_DOT;
    for (let index = 0; index < buckets.length; index += 1) {
      const representative = weightedPlane(buckets[index]);
      if (representative === null) continue;
      const compatibility = Math.abs(dot(representative.normal, sample.normal));
      if (compatibility >= bestDot) {
        bestDot = compatibility;
        bestIndex = index;
      }
    }
    if (bestIndex >= 0) {
      buckets[bestIndex].push(sample);
    } else if (buckets.length < 4) {
      buckets.push([sample]);
    }
  }

  return buckets
    .filter((bucket) => bucket.length >= MIN_CLUSTER_SAMPLES)
    .map((bucket) => weightedPlane(bucket))
    .filter((cluster): cluster is PlaneCluster => cluster !== null)
    .sort((left, right) => right.weight - left.weight);
}

function planesIndependent(a: PlaneCluster, b: PlaneCluster): boolean {
  return Math.abs(dot(a.normal, b.normal)) <= MIN_PLANE_SEPARATION_DOT;
}

function closestPointOnPlaneIntersection(
  first: PlaneCluster,
  second: PlaneCluster,
  target: ProjectPoint,
): ProjectPoint | null {
  const n1 = first.normal;
  const n2 = second.normal;
  const direction = cross(n1, n2);
  const directionSquared = dot(direction, direction);
  if (directionSquared < 1e-4) return null;

  const d1 = dot(n1, first.center);
  const d2 = dot(n2, second.center);
  const term1 = scale(cross(n2, direction), d1);
  const term2 = scale(cross(direction, n1), d2);
  const base = scale([
    term1[0] + term2[0],
    term1[1] + term2[1],
    term1[2] + term2[2],
  ], 1 / directionSquared);
  const normalizedDirection = normalize(direction);
  if (normalizedDirection === null) return null;
  const along = dot(subtract(target, base), normalizedDirection);
  return add(base, scale(normalizedDirection, along));
}

function intersectThreePlanes(
  first: PlaneCluster,
  second: PlaneCluster,
  third: PlaneCluster,
): ProjectPoint | null {
  const n1 = first.normal;
  const n2 = second.normal;
  const n3 = third.normal;
  const n2xn3 = cross(n2, n3);
  const determinant = dot(n1, n2xn3);
  if (Math.abs(determinant) < 0.12) return null;

  const d1 = dot(n1, first.center);
  const d2 = dot(n2, second.center);
  const d3 = dot(n3, third.center);
  const n3xn1 = cross(n3, n1);
  const n1xn2 = cross(n1, n2);
  return [
    (d1 * n2xn3[0] + d2 * n3xn1[0] + d3 * n1xn2[0]) / determinant,
    (d1 * n2xn3[1] + d2 * n3xn1[1] + d3 * n1xn2[1]) / determinant,
    (d1 * n2xn3[2] + d2 * n3xn1[2] + d3 * n1xn2[2]) / determinant,
  ];
}

function reject(message: string, supportCount: number, sourceDistanceMeters: number | null): ManualPlacementResolution {
  return {
    accepted: false,
    position: null,
    mode: null,
    confidence: 0,
    supportCount,
    sourceDistanceMeters,
    message,
  };
}

export function solveManualPlacementOnStableSurface(
  candidate: ProjectPoint,
  snapshot: ManualPlacementSurfaceSnapshot,
): ManualPlacementResolution {
  const count = Math.min(
    snapshot.pointCount,
    Math.floor(snapshot.positions.length / 3),
    Math.floor(snapshot.normals.length / 3),
    snapshot.confidence.length,
  );
  if (count < 4) {
    return reject('Scan this area from another angle before placing a corner.', count, null);
  }

  const searchRadius = clamp(snapshot.voxelSizeMeters * 5.5, 0.11, 0.22);
  const sourceGate = clamp(snapshot.voxelSizeMeters * 3.6, 0.075, 0.14);
  const local: Sample[] = [];
  let nearestDistance = Number.POSITIVE_INFINITY;

  for (let index = 0; index < count; index += 1) {
    const confidence = snapshot.confidence[index];
    if (!Number.isFinite(confidence) || confidence < MIN_SAMPLE_CONFIDENCE) continue;
    const offset = index * 3;
    const position: ProjectPoint = [
      snapshot.positions[offset],
      snapshot.positions[offset + 1],
      snapshot.positions[offset + 2],
    ];
    const sampleDistance = distance(candidate, position);
    nearestDistance = Math.min(nearestDistance, sampleDistance);
    if (sampleDistance > searchRadius) continue;
    const normal = normalize([
      snapshot.normals[offset],
      snapshot.normals[offset + 1],
      snapshot.normals[offset + 2],
    ]);
    if (normal === null) continue;
    local.push({ position, normal, confidence, distance: sampleDistance });
  }

  local.sort((left, right) => left.distance - right.distance);
  const samples = local.slice(0, MAX_LOCAL_SAMPLES);
  if (samples.length < 4 || nearestDistance > sourceGate) {
    return reject(
      'Depth under the cursor does not agree with the accumulated surface. Scan this edge again or move the aim slightly.',
      samples.length,
      Number.isFinite(nearestDistance) ? nearestDistance : null,
    );
  }

  const planes = clusterPlanes(samples);
  if (planes.length < 2) {
    return reject(
      'Only one stable surface is known here. Scan the corner from another side before placing it.',
      samples.length,
      nearestDistance,
    );
  }

  let bestPair: readonly [PlaneCluster, PlaneCluster] | null = null;
  for (let first = 0; first < planes.length; first += 1) {
    for (let second = first + 1; second < planes.length; second += 1) {
      if (!planesIndependent(planes[first], planes[second])) continue;
      bestPair = [planes[first], planes[second]];
      break;
    }
    if (bestPair !== null) break;
  }
  if (bestPair === null) {
    return reject(
      'The local observations do not define a stable edge yet. Scan from a wider angle.',
      samples.length,
      nearestDistance,
    );
  }

  let solved: ProjectPoint | null = null;
  let mode: ManualPlacementMode = 'edge';
  let usedWeight = bestPair[0].weight + bestPair[1].weight;

  for (let third = 0; third < planes.length; third += 1) {
    const plane = planes[third];
    if (plane === bestPair[0] || plane === bestPair[1]) continue;
    if (!planesIndependent(bestPair[0], plane) || !planesIndependent(bestPair[1], plane)) continue;
    const corner = intersectThreePlanes(bestPair[0], bestPair[1], plane);
    if (corner !== null && distance(corner, candidate) <= searchRadius * 1.35) {
      solved = corner;
      mode = 'corner';
      usedWeight += plane.weight;
      break;
    }
  }

  if (solved === null) {
    solved = closestPointOnPlaneIntersection(bestPair[0], bestPair[1], candidate);
  }
  if (solved === null || distance(solved, candidate) > searchRadius * 1.35) {
    return reject(
      'The detected edge is inconsistent with the cursor depth. Scan this corner again before placing it.',
      samples.length,
      nearestDistance,
    );
  }

  const solutionSupport = samples.reduce((minimum, sample) => Math.min(minimum, distance(sample.position, solved!)), Number.POSITIVE_INFINITY);
  if (!Number.isFinite(solutionSupport) || solutionSupport > searchRadius) {
    return reject('The solved corner is not supported by measured surface points.', samples.length, nearestDistance);
  }

  const density = clamp(samples.length / 12, 0, 1);
  const geometry = mode === 'corner' ? 1 : 0.82;
  const weightScore = clamp(usedWeight / 10, 0, 1);
  const confidence = clamp(density * 0.42 + geometry * 0.34 + weightScore * 0.24, 0, 1);
  return {
    accepted: true,
    position: solved,
    mode,
    confidence,
    supportCount: samples.length,
    sourceDistanceMeters: nearestDistance,
    message: mode === 'corner' ? 'Corner locked to stable multi-view geometry.' : 'Edge locked to stable multi-view geometry.',
  };
}

export function registerManualPlacementResolver(resolver: ManualPlacementResolver | null): void {
  activeResolver = resolver;
}

export function resolveManualPlacementCandidate(candidate: ProjectPoint): ManualPlacementResolution | null {
  return activeResolver?.(candidate) ?? null;
}
