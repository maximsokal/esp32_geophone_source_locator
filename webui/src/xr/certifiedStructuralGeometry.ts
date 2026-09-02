import { BoundedSpatialPointIndex } from './boundedSpatialPointIndex';
import {
  solveManualPlacementOnStableSurface,
  type ManualPlacementSurfaceSnapshot,
  type ProjectPoint,
} from './manualPlacementSurface';
import type {
  StructuralCornerCandidate,
  StructuralGuideViewpoint,
} from './structuralGuide';
import { estimateSurfaceFeatureComplexity, type SurfaceNormalEvidence } from './surfaceFeatureDensity';

const SEED_CELL_METERS = 0.13;
const MAX_SEEDS = 72;
const MAX_CORNERS = 8;
const FEATURE_INDEX_CELL_METERS = 0.09;
const FEATURE_QUERY_RADIUS_METERS = 0.18;
const MAX_FEATURE_EVIDENCE = 96;
const MIN_FEATURE_EVIDENCE = 10;
const MIN_SEED_FEATURE_SCORE = 0.20;
const MIN_SEED_ORIENTATION_FAMILIES = 2;
const MIN_CORNER_CONFIDENCE = 0.72;
const MAX_CORNER_UNCERTAINTY_METERS = 0.030;
const DEDUPE_METERS = 0.095;
const TRACK_RADIUS_METERS = 0.085;
const MIN_VIEW_TRANSLATION_METERS = 0.035;
const MIN_VIEW_BASELINE_METERS = 0.085;
const MIN_VIEW_RAY_ANGLE_RADIANS = (9 * Math.PI) / 180;
const MAX_TRACK_EVIDENCE = 6;
const MIN_MATURE_OBSERVATIONS = 2;
const MAX_TRACK_SPREAD_METERS = 0.035;
const STALE_TRACK_KEYFRAMES = 12;
const TRACK_SMOOTHING = 0.34;

interface Seed {
  position: ProjectPoint;
  confidence: number;
  count: number;
}

interface CornerEvidence {
  position: ProjectPoint;
  camera: ProjectPoint;
  ray: readonly [number, number, number];
  keyframeId: number;
}

interface CornerTrack {
  id: number;
  position: ProjectPoint;
  confidence: number;
  support: number;
  lastSeenKeyframe: number;
  evidence: CornerEvidence[];
}

function key(point: ProjectPoint): string {
  return `${Math.floor(point[0] / SEED_CELL_METERS)}:${Math.floor(point[1] / SEED_CELL_METERS)}:${Math.floor(point[2] / SEED_CELL_METERS)}`;
}

function distance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function cornerId(position: ProjectPoint): string {
  const q = DEDUPE_METERS;
  return `robust:${Math.round(position[0] / q)}:${Math.round(position[1] / q)}:${Math.round(position[2] / q)}`;
}

function normalize(vector: readonly [number, number, number]): readonly [number, number, number] | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return Number.isFinite(magnitude) && magnitude > 1e-9
    ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude]
    : null;
}

function dot(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function optionalAt(array: ArrayLike<number> | undefined, index: number, fallback: number): number {
  if (array === undefined || index >= array.length) return fallback;
  const value = Number(array[index]);
  return Number.isFinite(value) ? value : fallback;
}

function evidenceFrom(
  candidate: ProjectPoint,
  viewpoint: StructuralGuideViewpoint | null | undefined,
): CornerEvidence | null {
  if (viewpoint === null || viewpoint === undefined || !Number.isFinite(viewpoint.keyframeId)) return null;
  const ray = normalize([
    candidate[0] - viewpoint.position[0],
    candidate[1] - viewpoint.position[1],
    candidate[2] - viewpoint.position[2],
  ]);
  if (ray === null) return null;
  if (viewpoint.forward !== undefined) {
    const forward = normalize(viewpoint.forward);
    if (forward !== null && dot(forward, ray) <= 0.55) return null;
  }
  return { position: candidate, camera: viewpoint.position, ray, keyframeId: viewpoint.keyframeId };
}

function independentEvidence(existing: readonly CornerEvidence[], next: CornerEvidence): boolean {
  if (existing.some((item) => item.keyframeId === next.keyframeId)) return false;
  if (existing.length === 0) return true;
  for (const previous of existing) {
    const baseline = distance(previous.camera, next.camera);
    if (baseline < MIN_VIEW_TRANSLATION_METERS) return false;
    const rayDot = Math.max(-1, Math.min(1, dot(previous.ray, next.ray)));
    const angle = Math.acos(rayDot);
    if (baseline < MIN_VIEW_BASELINE_METERS && angle < MIN_VIEW_RAY_ANGLE_RADIANS) return false;
  }
  return true;
}

function evidenceSpread(evidence: readonly CornerEvidence[]): number {
  if (evidence.length <= 1) return 0;
  const mean: ProjectPoint = [
    evidence.reduce((sum, item) => sum + item.position[0], 0) / evidence.length,
    evidence.reduce((sum, item) => sum + item.position[1], 0) / evidence.length,
    evidence.reduce((sum, item) => sum + item.position[2], 0) / evidence.length,
  ];
  return Math.sqrt(
    evidence.reduce((sum, item) => sum + distance(item.position, mean) ** 2, 0) / evidence.length,
  );
}

function boundedFeatureIndices(indices: readonly number[]): readonly number[] {
  if (indices.length <= MAX_FEATURE_EVIDENCE) return indices;
  const sampled = new Array<number>(MAX_FEATURE_EVIDENCE);
  const stride = indices.length / MAX_FEATURE_EVIDENCE;
  for (let index = 0; index < MAX_FEATURE_EVIDENCE; index += 1) {
    sampled[index] = indices[Math.min(indices.length - 1, Math.floor((index + 0.5) * stride))];
  }
  return sampled;
}

function structuralFeatureNearSeed(
  snapshot: ManualPlacementSurfaceSnapshot,
  spatialIndex: BoundedSpatialPointIndex,
  seed: Seed,
): boolean {
  const usable = spatialIndex.queryRadius(seed.position, FEATURE_QUERY_RADIUS_METERS).filter((index) => {
    const confidence = optionalAt(snapshot.confidence, index, 0);
    const ready = optionalAt(snapshot.coverageReady, index, 1) > 0;
    const residual = optionalAt(snapshot.residualMeters, index, 0.012);
    const normalConfidence = optionalAt(snapshot.normalConfidence, index, confidence);
    return ready && confidence >= 0.62 && residual <= 0.040 && normalConfidence >= 0.22;
  });
  if (usable.length < MIN_FEATURE_EVIDENCE) return false;

  const evidence: SurfaceNormalEvidence[] = [];
  for (const index of boundedFeatureIndices(usable)) {
    const offset = index * 3;
    evidence.push({
      normalX: snapshot.normals[offset],
      normalY: snapshot.normals[offset + 1],
      normalZ: snapshot.normals[offset + 2],
      normalConfidence: optionalAt(snapshot.normalConfidence, index, snapshot.confidence[index]),
      x: snapshot.positions[offset],
      y: snapshot.positions[offset + 1],
      z: snapshot.positions[offset + 2],
    });
  }
  const feature = estimateSurfaceFeatureComplexity(evidence);
  return feature.spatiallySupportedFamilies >= MIN_SEED_ORIENTATION_FAMILIES && feature.score >= MIN_SEED_FEATURE_SCORE;
}

/**
 * Spatially sparse robust corner discovery on the already fused surface map.
 *
 * One strong seed per 13-cm cell is considered, but flat cells are discarded by
 * bounded local normal-family evidence before the expensive uncertainty solver
 * scans the surface. Two spatially supported orientation families are sufficient
 * for this prefilter: requiring three here would miss real corners with a depth
 * hole at the vertex. The robust solver remains authoritative and still requires
 * THREE supported planes, bounded uncertainty and measured support near the
 * solved intersection.
 *
 * This detector certifies geometry only. Candidate-specific temporal maturity is
 * handled by CertifiedStructuralCornerTracker; a single analysis is exactly one
 * physical observation and is never presented as multi-view persistence.
 */
export function detectCertifiedStructuralCorners(
  snapshot: ManualPlacementSurfaceSnapshot,
): readonly StructuralCornerCandidate[] {
  const count = Math.min(
    snapshot.pointCount,
    Math.floor(snapshot.positions.length / 3),
    Math.floor(snapshot.normals.length / 3),
    snapshot.confidence.length,
  );
  if (count < 24) return [];

  const cells = new Map<string, Seed>();
  for (let index = 0; index < count; index += 1) {
    const confidence = snapshot.confidence[index];
    if (!Number.isFinite(confidence) || confidence < 0.62) continue;
    if (snapshot.coverageReady !== undefined && index < snapshot.coverageReady.length && snapshot.coverageReady[index] === 0) continue;
    const offset = index * 3;
    const position: ProjectPoint = [
      snapshot.positions[offset],
      snapshot.positions[offset + 1],
      snapshot.positions[offset + 2],
    ];
    const cellKey = key(position);
    const existing = cells.get(cellKey);
    if (existing === undefined) {
      cells.set(cellKey, { position, confidence, count: 1 });
      continue;
    }
    existing.count += 1;
    if (confidence > existing.confidence) {
      existing.position = position;
      existing.confidence = confidence;
    }
  }

  const seeds = [...cells.values()]
    .filter((seed) => seed.count >= 2)
    .sort((left, right) => {
      const leftScore = left.confidence + Math.min(8, left.count) * 0.012;
      const rightScore = right.confidence + Math.min(8, right.count) * 0.012;
      return rightScore - leftScore;
    })
    .slice(0, MAX_SEEDS);
  const spatialIndex = new BoundedSpatialPointIndex(snapshot.positions, count, FEATURE_INDEX_CELL_METERS);

  const corners: StructuralCornerCandidate[] = [];
  for (const seed of seeds) {
    if (!structuralFeatureNearSeed(snapshot, spatialIndex, seed)) continue;
    const result = solveManualPlacementOnStableSurface(seed.position, snapshot);
    if (
      !result.accepted ||
      result.mode !== 'corner' ||
      result.position === null ||
      result.confidence < MIN_CORNER_CONFIDENCE ||
      (result.uncertaintyMeters ?? Number.POSITIVE_INFINITY) > MAX_CORNER_UNCERTAINTY_METERS
    ) continue;

    const existing = corners.find((corner) => distance(corner.position, result.position!) <= DEDUPE_METERS);
    const candidate: StructuralCornerCandidate = {
      id: cornerId(result.position),
      kind: 'corner',
      position: result.position,
      confidence: result.confidence,
      geometryConfidence: result.confidence,
      support: result.supportCount,
      observations: 1,
    };
    if (existing === undefined) {
      corners.push(candidate);
    } else if (candidate.confidence > existing.confidence) {
      const index = corners.indexOf(existing);
      corners[index] = candidate;
    }
  }

  return corners
    .sort((left, right) => right.confidence - left.confidence)
    .slice(0, MAX_CORNERS);
}

/**
 * Candidate-specific multi-view tracker for uncertainty-certified three-plane
 * corners. Geometry can be re-solved every analysis, but maturity only advances
 * when this SAME spatial candidate is observed from an independent camera pose.
 */
export class CertifiedStructuralCornerTracker {
  private tracks: CornerTrack[] = [];
  private nextId = 1;

  reset(): void {
    this.tracks = [];
    this.nextId = 1;
  }

  update(
    snapshot: ManualPlacementSurfaceSnapshot,
    viewpoint: StructuralGuideViewpoint | null | undefined,
  ): readonly StructuralCornerCandidate[] {
    const candidates = detectCertifiedStructuralCorners(snapshot);
    const keyframeId = viewpoint?.keyframeId ?? 0;
    const matchedTrackIds = new Set<number>();

    for (const candidate of candidates) {
      let best: CornerTrack | null = null;
      let bestDistance = Number.POSITIVE_INFINITY;
      for (const track of this.tracks) {
        if (matchedTrackIds.has(track.id)) continue;
        const candidateDistance = distance(track.position, candidate.position);
        if (candidateDistance > TRACK_RADIUS_METERS || candidateDistance >= bestDistance) continue;
        best = track;
        bestDistance = candidateDistance;
      }

      const nextEvidence = evidenceFrom(candidate.position, viewpoint);
      if (best === null) {
        const evidence = nextEvidence === null ? [] : [nextEvidence];
        const created: CornerTrack = {
          id: this.nextId++,
          position: candidate.position,
          confidence: candidate.confidence,
          support: candidate.support,
          lastSeenKeyframe: keyframeId,
          evidence,
        };
        this.tracks.push(created);
        matchedTrackIds.add(created.id);
        continue;
      }

      matchedTrackIds.add(best.id);
      best.lastSeenKeyframe = keyframeId;
      best.position = [
        best.position[0] + (candidate.position[0] - best.position[0]) * TRACK_SMOOTHING,
        best.position[1] + (candidate.position[1] - best.position[1]) * TRACK_SMOOTHING,
        best.position[2] + (candidate.position[2] - best.position[2]) * TRACK_SMOOTHING,
      ];
      best.confidence = Math.max(best.confidence * 0.86, candidate.confidence);
      best.support = Math.max(best.support, candidate.support);
      if (nextEvidence !== null && independentEvidence(best.evidence, nextEvidence)) {
        best.evidence.push(nextEvidence);
        if (best.evidence.length > MAX_TRACK_EVIDENCE) best.evidence.shift();
      }
    }

    if (keyframeId > 0) {
      this.tracks = this.tracks.filter((track) => keyframeId - track.lastSeenKeyframe <= STALE_TRACK_KEYFRAMES);
    }

    return this.tracks
      .filter((track) =>
        matchedTrackIds.has(track.id) &&
        track.evidence.length >= MIN_MATURE_OBSERVATIONS &&
        evidenceSpread(track.evidence) <= MAX_TRACK_SPREAD_METERS &&
        track.confidence >= MIN_CORNER_CONFIDENCE
      )
      .map((track): StructuralCornerCandidate => ({
        id: `robust-track:${track.id}`,
        kind: 'corner',
        position: track.position,
        confidence: Math.min(0.96, track.confidence),
        geometryConfidence: track.confidence,
        support: track.support,
        observations: track.evidence.length,
      }))
      .sort((left, right) => right.confidence - left.confidence)
      .slice(0, MAX_CORNERS);
  }
}
