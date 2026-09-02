import type { ManualPlacementSurfaceSnapshot, ProjectPoint, ProjectVector } from './manualPlacementSurface';
import {
  distance3,
  dot3,
  extractRobustPlanes,
  intersectTwoRobustPlanes,
  normalize3,
  type RobustPlane,
  type RobustPlaneSample,
} from './robustPlaneGeometry';
import type { StructuralCornerCandidate, StructuralGuideViewpoint } from './structuralGuide';

export interface EdgeEndpointSnapshot extends ManualPlacementSurfaceSnapshot {
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
}

interface LocalSample extends RobustPlaneSample {
  sourceIndex: number;
}

export interface EdgeEndpointObservation {
  position: ProjectPoint;
  direction: ProjectVector;
  confidence: number;
  support: number;
  freeBeyond: boolean;
  uncertaintyMeters: number;
}

interface EndpointEvidence {
  position: ProjectPoint;
  camera: ProjectPoint;
  ray: ProjectVector;
  keyframeId: number;
  freeBeyond: boolean;
}

interface EndpointTrack {
  id: number;
  position: ProjectPoint;
  direction: ProjectVector;
  confidence: number;
  support: number;
  observations: number;
  lastSeenKeyframe: number;
  freeVotes: number;
  evidence: EndpointEvidence[];
}

const SEED_CELL_METERS = 0.14;
const MAX_SEEDS = 72;
const LOCAL_RADIUS_METERS = 0.18;
const EDGE_BAND_METERS = 0.065;
const MIN_PLANE_SUPPORT = 7;
const MIN_EDGE_SIDE_SUPPORT = 4;
const MIN_EDGE_LENGTH_METERS = 0.085;
const ENDPOINT_INSIDE_WINDOW_METERS = 0.075;
const ENDPOINT_OUTSIDE_GAP_METERS = 0.045;
const ENDPOINT_FREE_PROBES_METERS = [0.045, 0.085, 0.125] as const;
const TRACK_RADIUS_METERS = 0.075;
const TRACK_DIRECTION_DOT = Math.cos((22 * Math.PI) / 180);
const MIN_VIEW_TRANSLATION_METERS = 0.035;
const MIN_VIEW_BASELINE_METERS = 0.09;
const MIN_VIEW_RAY_ANGLE = (9 * Math.PI) / 180;
const MIN_MATURE_OBSERVATIONS = 3;
const MIN_FREE_VOTES = 2;
const MAX_ENDPOINT_SPREAD_METERS = 0.032;
const STALE_KEYFRAMES = 12;
const MAX_EVIDENCE = 7;
const MAX_OUTPUT_ENDPOINTS = 6;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

function subtract(a: ProjectPoint, b: ProjectPoint): ProjectVector {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function addScaled(point: ProjectPoint, direction: ProjectVector, amount: number): ProjectPoint {
  return [
    point[0] + direction[0] * amount,
    point[1] + direction[1] * amount,
    point[2] + direction[2] * amount,
  ];
}

function canonicalDirection(direction: ProjectVector): ProjectVector {
  const n = normalize3(direction) ?? [0, 0, 1] as ProjectVector;
  const ax = Math.abs(n[0]);
  const ay = Math.abs(n[1]);
  const az = Math.abs(n[2]);
  const axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
  return n[axis] < 0 ? [-n[0], -n[1], -n[2]] : n;
}

function lineDistance(point: ProjectPoint, origin: ProjectPoint, direction: ProjectVector): number {
  const delta = subtract(point, origin);
  const along = dot3(delta, direction);
  const closest = addScaled(origin, direction, along);
  return distance3(point, closest);
}

function quantile(values: readonly number[], q: number): number {
  if (values.length === 0) return Number.NaN;
  const sorted = [...values].sort((a, b) => a - b);
  const position = clamp(q, 0, 1) * (sorted.length - 1);
  const lower = Math.floor(position);
  const upper = Math.ceil(position);
  if (lower === upper) return sorted[lower];
  const fraction = position - lower;
  return sorted[lower] * (1 - fraction) + sorted[upper] * fraction;
}

function optionalAt(array: ArrayLike<number> | undefined, index: number, fallback: number): number {
  if (array === undefined || index >= array.length) return fallback;
  const value = Number(array[index]);
  return Number.isFinite(value) ? value : fallback;
}

function evidenceWeight(snapshot: EdgeEndpointSnapshot, index: number): number {
  const confidence = optionalAt(snapshot.confidence, index, 0);
  const residual = optionalAt(snapshot.residualMeters, index, 0.018);
  const normal = optionalAt(snapshot.normalConfidence, index, 0.7);
  const coverage = optionalAt(snapshot.coverageConfidence, index, confidence);
  const ready = optionalAt(snapshot.coverageReady, index, 1) > 0;
  if (!ready || confidence < 0.62 || residual > 0.038 || normal < 0.35) return 0;
  const residualScore = clamp01((0.038 - residual) / 0.032);
  return clamp01(confidence * 0.38 + coverage * 0.24 + residualScore * 0.22 + normal * 0.16);
}

function freeKey(point: ProjectPoint, size: number): string {
  return `${Math.floor(point[0] / size)}:${Math.floor(point[1] / size)}:${Math.floor(point[2] / size)}`;
}

function buildFreeIndex(snapshot: EdgeEndpointSnapshot): { size: number; keys: Set<string> } | null {
  const positions = snapshot.freeSpacePositions;
  const size = snapshot.freeSpaceVoxelSizeMeters;
  if (positions === undefined || size === undefined || !Number.isFinite(size) || size <= 0) return null;
  const keys = new Set<string>();
  for (let offset = 0; offset + 2 < positions.length; offset += 3) {
    keys.add(freeKey([positions[offset], positions[offset + 1], positions[offset + 2]], size));
  }
  return { size, keys };
}

function verifiedFreeBeyond(
  endpoint: ProjectPoint,
  direction: ProjectVector,
  sign: -1 | 1,
  index: { size: number; keys: Set<string> } | null,
): boolean {
  if (index === null) return false;
  let votes = 0;
  for (const distance of ENDPOINT_FREE_PROBES_METERS) {
    const probe = addScaled(endpoint, direction, distance * sign);
    if (index.keys.has(freeKey(probe, index.size))) votes += 1;
  }
  return votes >= 2;
}

function supportParameters(
  plane: RobustPlane,
  samples: readonly LocalSample[],
  origin: ProjectPoint,
  direction: ProjectVector,
): number[] {
  const parameters: number[] = [];
  for (const localIndex of plane.inlierIndices) {
    const sample = samples[localIndex];
    if (sample === undefined) continue;
    if (lineDistance(sample.position, origin, direction) > EDGE_BAND_METERS) continue;
    parameters.push(dot3(subtract(sample.position, origin), direction));
  }
  return parameters;
}

function endpointSupportCount(values: readonly number[], endpoint: number, sign: -1 | 1): number {
  let support = 0;
  for (const value of values) {
    const inward = (value - endpoint) * -sign;
    if (inward >= -0.012 && inward <= ENDPOINT_INSIDE_WINDOW_METERS) support += 1;
  }
  return support;
}

function hasMeasuredSupportBeyond(values: readonly number[], endpoint: number, sign: -1 | 1): boolean {
  return values.some((value) => (value - endpoint) * sign >= ENDPOINT_OUTSIDE_GAP_METERS);
}

function makeEndpointObservation(
  origin: ProjectPoint,
  direction: ProjectVector,
  parameter: number,
  sign: -1 | 1,
  firstValues: readonly number[],
  secondValues: readonly number[],
  first: RobustPlane,
  second: RobustPlane,
  freeIndex: { size: number; keys: Set<string> } | null,
): EdgeEndpointObservation | null {
  const firstSupport = endpointSupportCount(firstValues, parameter, sign);
  const secondSupport = endpointSupportCount(secondValues, parameter, sign);
  if (firstSupport < MIN_EDGE_SIDE_SUPPORT || secondSupport < MIN_EDGE_SIDE_SUPPORT) return null;
  if (hasMeasuredSupportBeyond(firstValues, parameter, sign) || hasMeasuredSupportBeyond(secondValues, parameter, sign)) return null;

  const position = addScaled(origin, direction, parameter);
  const freeBeyond = verifiedFreeBeyond(position, direction, sign, freeIndex);
  const support = firstSupport + secondSupport;
  const supportScore = clamp01(support / 14);
  const planeScore = Math.min(first.confidence, second.confidence);
  const uncertaintyMeters = Math.max(first.offsetSigmaMeters, second.offsetSigmaMeters);
  const uncertaintyScore = clamp01((0.035 - uncertaintyMeters) / 0.028);
  const confidence = Math.min(0.96, clamp01(
    planeScore * 0.44 + supportScore * 0.24 + uncertaintyScore * 0.18 + (freeBeyond ? 0.14 : 0),
  ));
  return {
    position,
    direction: canonicalDirection(direction),
    confidence,
    support,
    freeBeyond,
    uncertaintyMeters,
  };
}

function seedKey(point: ProjectPoint): string {
  return `${Math.floor(point[0] / SEED_CELL_METERS)}:${Math.floor(point[1] / SEED_CELL_METERS)}:${Math.floor(point[2] / SEED_CELL_METERS)}`;
}

/**
 * Detect finite endpoints of a robust two-plane intersection.
 *
 * A two-plane intersection alone defines an infinite 3D line, not a corner.
 * We therefore estimate only the interval where BOTH measured planes have
 * near-line support. An endpoint is retained only when support exists on the
 * inside, no measured continuation exists outside, and ray-verified FREE can
 * be observed beyond the endpoint. Multi-view persistence is handled by the
 * tracker below; a single frame never becomes an automatic corner.
 */
export function detectEdgeEndpointObservations(snapshot: EdgeEndpointSnapshot): readonly EdgeEndpointObservation[] {
  const count = Math.min(
    snapshot.pointCount,
    Math.floor(snapshot.positions.length / 3),
    Math.floor(snapshot.normals.length / 3),
    snapshot.confidence.length,
  );
  if (count < 24) return [];

  const seeds = new Map<string, { position: ProjectPoint; confidence: number }>();
  for (let index = 0; index < count; index += 1) {
    const weight = evidenceWeight(snapshot, index);
    if (weight <= 0.05) continue;
    const offset = index * 3;
    const position: ProjectPoint = [snapshot.positions[offset], snapshot.positions[offset + 1], snapshot.positions[offset + 2]];
    const key = seedKey(position);
    const existing = seeds.get(key);
    if (existing === undefined || weight > existing.confidence) seeds.set(key, { position, confidence: weight });
  }

  const seedList = [...seeds.values()]
    .sort((left, right) => right.confidence - left.confidence)
    .slice(0, MAX_SEEDS);
  const freeIndex = buildFreeIndex(snapshot);
  const observations: EdgeEndpointObservation[] = [];

  for (const seed of seedList) {
    const local: LocalSample[] = [];
    for (let index = 0; index < count; index += 1) {
      const offset = index * 3;
      const position: ProjectPoint = [snapshot.positions[offset], snapshot.positions[offset + 1], snapshot.positions[offset + 2]];
      if (distance3(position, seed.position) > LOCAL_RADIUS_METERS) continue;
      const weight = evidenceWeight(snapshot, index);
      if (weight <= 0.05) continue;
      local.push({
        sourceIndex: index,
        position,
        normal: [snapshot.normals[offset], snapshot.normals[offset + 1], snapshot.normals[offset + 2]],
        weight,
        residualMeters: optionalAt(snapshot.residualMeters, index, 0.018),
        normalConfidence: optionalAt(snapshot.normalConfidence, index, 0.7),
      });
    }
    if (local.length < MIN_PLANE_SUPPORT * 2) continue;

    const planes = extractRobustPlanes(local, {
      maxPlanes: 3,
      minSupport: MIN_PLANE_SUPPORT,
      inlierThresholdMeters: clamp(snapshot.voxelSizeMeters * 0.40, 0.011, 0.020),
      normalGateRadians: (38 * Math.PI) / 180,
      minimumTangentSpreadMeters: clamp(snapshot.voxelSizeMeters * 0.55, 0.020, 0.032),
    });
    if (planes.length !== 2) continue;

    const edge = intersectTwoRobustPlanes(planes[0], planes[1], seed.position);
    if (edge === null || edge.uncertaintyMeters > 0.035) continue;
    const direction = canonicalDirection(edge.direction);
    const firstValues = supportParameters(planes[0], local, edge.point, direction);
    const secondValues = supportParameters(planes[1], local, edge.point, direction);
    if (firstValues.length < MIN_EDGE_SIDE_SUPPORT || secondValues.length < MIN_EDGE_SIDE_SUPPORT) continue;

    const firstLow = quantile(firstValues, 0.10);
    const firstHigh = quantile(firstValues, 0.90);
    const secondLow = quantile(secondValues, 0.10);
    const secondHigh = quantile(secondValues, 0.90);
    const low = Math.max(firstLow, secondLow);
    const high = Math.min(firstHigh, secondHigh);
    if (!Number.isFinite(low) || !Number.isFinite(high) || high - low < MIN_EDGE_LENGTH_METERS) continue;

    for (const [parameter, sign] of [[low, -1], [high, 1]] as const) {
      const observation = makeEndpointObservation(
        edge.point,
        direction,
        parameter,
        sign,
        firstValues,
        secondValues,
        planes[0],
        planes[1],
        freeIndex,
      );
      if (observation === null) continue;
      const duplicate = observations.find((candidate) => distance3(candidate.position, observation.position) <= 0.055);
      if (duplicate === undefined) {
        observations.push(observation);
      } else if (observation.confidence > duplicate.confidence) {
        const index = observations.indexOf(duplicate);
        observations[index] = observation;
      }
    }
  }

  return observations
    .sort((left, right) => right.confidence - left.confidence)
    .slice(0, 12);
}

function rayFrom(viewpoint: StructuralGuideViewpoint, position: ProjectPoint): ProjectVector | null {
  return normalize3(subtract(position, viewpoint.position));
}

function independentEvidence(track: EndpointTrack, viewpoint: StructuralGuideViewpoint, position: ProjectPoint): boolean {
  const ray = rayFrom(viewpoint, position);
  if (ray === null) return false;
  for (const evidence of track.evidence) {
    if (evidence.keyframeId === viewpoint.keyframeId) return false;
    const baseline = distance3(evidence.camera, viewpoint.position);
    if (baseline < MIN_VIEW_TRANSLATION_METERS) return false;
    const cosine = clamp(dot3(evidence.ray, ray), -1, 1);
    const angle = Math.acos(cosine);
    if (baseline < MIN_VIEW_BASELINE_METERS && angle < MIN_VIEW_RAY_ANGLE) return false;
  }
  return true;
}

function endpointSpread(evidence: readonly EndpointEvidence[]): number {
  if (evidence.length <= 1) return 0;
  const mean: ProjectPoint = [
    evidence.reduce((sum, item) => sum + item.position[0], 0) / evidence.length,
    evidence.reduce((sum, item) => sum + item.position[1], 0) / evidence.length,
    evidence.reduce((sum, item) => sum + item.position[2], 0) / evidence.length,
  ];
  const squared = evidence.reduce((sum, item) => sum + distance3(item.position, mean) ** 2, 0) / evidence.length;
  return Math.sqrt(squared);
}

export class StructuralEdgeEndpointTracker {
  private tracks: EndpointTrack[] = [];
  private nextId = 1;

  reset(): void {
    this.tracks = [];
    this.nextId = 1;
  }

  update(
    snapshot: EdgeEndpointSnapshot,
    viewpoint: StructuralGuideViewpoint | null | undefined,
  ): readonly StructuralCornerCandidate[] {
    const observations = detectEdgeEndpointObservations(snapshot);
    const keyframeId = viewpoint?.keyframeId ?? snapshot.pointCount;
    const matchedTrackIds = new Set<number>();

    for (const observation of observations) {
      let best: EndpointTrack | null = null;
      let bestDistance = Number.POSITIVE_INFINITY;
      for (const track of this.tracks) {
        if (matchedTrackIds.has(track.id)) continue;
        const positionDistance = distance3(track.position, observation.position);
        const directionDot = Math.abs(dot3(track.direction, observation.direction));
        if (positionDistance > TRACK_RADIUS_METERS || directionDot < TRACK_DIRECTION_DOT) continue;
        if (positionDistance < bestDistance) {
          best = track;
          bestDistance = positionDistance;
        }
      }

      if (best === null) {
        const ray = viewpoint === null || viewpoint === undefined ? null : rayFrom(viewpoint, observation.position);
        const evidence: EndpointEvidence[] = [];
        if (viewpoint !== null && viewpoint !== undefined && ray !== null) {
          evidence.push({
            position: observation.position,
            camera: viewpoint.position,
            ray,
            keyframeId: viewpoint.keyframeId,
            freeBeyond: observation.freeBeyond,
          });
        }
        const created: EndpointTrack = {
          id: this.nextId++,
          position: observation.position,
          direction: observation.direction,
          confidence: observation.confidence,
          support: observation.support,
          observations: evidence.length,
          lastSeenKeyframe: keyframeId,
          freeVotes: observation.freeBeyond && evidence.length > 0 ? 1 : 0,
          evidence,
        };
        this.tracks.push(created);
        matchedTrackIds.add(created.id);
        continue;
      }

      matchedTrackIds.add(best.id);
      best.lastSeenKeyframe = keyframeId;
      best.position = [
        best.position[0] * 0.58 + observation.position[0] * 0.42,
        best.position[1] * 0.58 + observation.position[1] * 0.42,
        best.position[2] * 0.58 + observation.position[2] * 0.42,
      ];
      best.direction = canonicalDirection([
        best.direction[0] + observation.direction[0],
        best.direction[1] + observation.direction[1],
        best.direction[2] + observation.direction[2],
      ]);
      best.confidence = Math.max(best.confidence * 0.82, observation.confidence);
      best.support = Math.max(best.support, observation.support);

      if (viewpoint !== null && viewpoint !== undefined && independentEvidence(best, viewpoint, observation.position)) {
        const ray = rayFrom(viewpoint, observation.position);
        if (ray !== null) {
          best.evidence.push({
            position: observation.position,
            camera: viewpoint.position,
            ray,
            keyframeId: viewpoint.keyframeId,
            freeBeyond: observation.freeBeyond,
          });
          if (best.evidence.length > MAX_EVIDENCE) best.evidence.shift();
          best.observations = best.evidence.length;
          best.freeVotes = best.evidence.filter((item) => item.freeBeyond).length;
        }
      }
    }

    this.tracks = this.tracks.filter((track) => keyframeId - track.lastSeenKeyframe <= STALE_KEYFRAMES);

    return this.tracks
      .filter((track) => {
        if (track.observations < MIN_MATURE_OBSERVATIONS || track.freeVotes < MIN_FREE_VOTES) return false;
        if (endpointSpread(track.evidence) > MAX_ENDPOINT_SPREAD_METERS) return false;
        return track.confidence >= 0.70;
      })
      .map((track): StructuralCornerCandidate => {
        const spread = endpointSpread(track.evidence);
        const persistenceScore = clamp01((track.observations - 2) / 3);
        const freeScore = clamp01(track.freeVotes / Math.max(1, track.observations));
        const spreadScore = clamp01((MAX_ENDPOINT_SPREAD_METERS - spread) / MAX_ENDPOINT_SPREAD_METERS);
        const confidence = Math.min(0.94, clamp01(
          track.confidence * 0.50 + persistenceScore * 0.18 + freeScore * 0.18 + spreadScore * 0.14,
        ));
        return {
          id: `endpoint:${track.id}`,
          kind: 'corner',
          position: track.position,
          confidence,
          geometryConfidence: track.confidence,
          support: track.support,
          observations: track.observations,
        };
      })
      .sort((left, right) => right.confidence - left.confidence)
      .slice(0, MAX_OUTPUT_ENDPOINTS);
  }
}
