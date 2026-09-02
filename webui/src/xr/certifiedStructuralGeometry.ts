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
const SEED_REGION_METERS = 0.52;
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
const CORNER_ID_QUANTIZATION_METERS = 0.040;
const MIN_CORNER_ASSOCIATION_METERS = 0.035;
const MAX_CORNER_ASSOCIATION_METERS = 0.060;
const CORNER_ASSOCIATION_DRIFT_METERS = 0.012;
const CORNER_ASSOCIATION_UNCERTAINTY_FACTOR = 1.5;
const CORNER_ASSOCIATION_ARM_FRACTION = 0.95;
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

interface CertifiedCornerObservation extends StructuralCornerCandidate {
  uncertaintyMeters: number;
  minimumArmSpanMeters: number;
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
  uncertaintyMeters: number;
  minimumArmSpanMeters: number;
  lastSeenKeyframe: number;
  evidence: CornerEvidence[];
}

function key(point: ProjectPoint): string {
  return `${Math.floor(point[0] / SEED_CELL_METERS)}:${Math.floor(point[1] / SEED_CELL_METERS)}:${Math.floor(point[2] / SEED_CELL_METERS)}`;
}

function regionKey(point: ProjectPoint): string {
  return `${Math.floor(point[0] / SEED_REGION_METERS)}:${Math.floor(point[1] / SEED_REGION_METERS)}:${Math.floor(point[2] / SEED_REGION_METERS)}`;
}

function distance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function comparePoint(left: ProjectPoint, right: ProjectPoint): number {
  return left[0] - right[0] || left[1] - right[1] || left[2] - right[2];
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function seedScore(seed: Seed): number {
  return seed.confidence + Math.min(8, seed.count) * 0.012;
}

function compareSeeds(left: Seed, right: Seed): number {
  return seedScore(right) - seedScore(left) || comparePoint(left.position, right.position);
}

/**
 * Keep the global solver budget bounded without allowing one large, high-quality
 * flat region to consume all 72 seeds before feature rejection. Round-robin over
 * coarse spatial regions preserves broad object coverage. A small scan with only
 * one region still receives the entire budget, so this is fairness rather than a
 * per-region quality cap.
 */
function spatiallyFairSeeds(candidates: readonly Seed[]): Seed[] {
  const regions = new Map<string, Seed[]>();
  for (const seed of candidates) {
    const region = regionKey(seed.position);
    const bucket = regions.get(region) ?? [];
    bucket.push(seed);
    regions.set(region, bucket);
  }
  const orderedRegions = [...regions.entries()]
    .map(([region, seeds]) => ({ region, seeds: seeds.sort(compareSeeds), cursor: 0 }))
    .sort((left, right) => left.region.localeCompare(right.region));
  const output: Seed[] = [];
  let advanced = true;
  while (output.length < MAX_SEEDS && advanced) {
    advanced = false;
    for (const region of orderedRegions) {
      if (output.length >= MAX_SEEDS) break;
      if (region.cursor >= region.seeds.length) continue;
      output.push(region.seeds[region.cursor++]);
      advanced = true;
    }
  }
  return output;
}

function cornerId(position: ProjectPoint): string {
  const q = CORNER_ID_QUANTIZATION_METERS;
  return `robust:${Math.round(position[0] / q)}:${Math.round(position[1] / q)}:${Math.round(position[2] / q)}`;
}

/**
 * Spatial association is bounded by BOTH numerical uncertainty and measured
 * physical scale. A noisy solve may move slightly between views, but a universal
 * 8-10 cm radius must not merge two distinct small structural corners.
 */
export function certifiedCornerAssociationGateMeters(
  firstUncertaintyMeters: number,
  secondUncertaintyMeters: number,
  firstMinimumArmSpanMeters: number,
  secondMinimumArmSpanMeters: number,
): number {
  const uncertainty = Math.max(
    Number.isFinite(firstUncertaintyMeters) ? Math.max(0, firstUncertaintyMeters) : 0,
    Number.isFinite(secondUncertaintyMeters) ? Math.max(0, secondUncertaintyMeters) : 0,
  );
  const uncertaintyGate =
    CORNER_ASSOCIATION_DRIFT_METERS +
    Math.min(MAX_CORNER_UNCERTAINTY_METERS, uncertainty) * CORNER_ASSOCIATION_UNCERTAINTY_FACTOR;
  const armCandidates = [firstMinimumArmSpanMeters, secondMinimumArmSpanMeters]
    .filter((value) => Number.isFinite(value) && value > 0);
  const armGate = armCandidates.length > 0
    ? Math.min(...armCandidates) * CORNER_ASSOCIATION_ARM_FRACTION
    : MAX_CORNER_ASSOCIATION_METERS;
  return clamp(
    Math.min(uncertaintyGate, armGate),
    MIN_CORNER_ASSOCIATION_METERS,
    MAX_CORNER_ASSOCIATION_METERS,
  );
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

function compareFeatureIndices(
  snapshot: ManualPlacementSurfaceSnapshot,
  leftIndex: number,
  rightIndex: number,
): number {
  const left = leftIndex * 3;
  const right = rightIndex * 3;
  return snapshot.positions[left] - snapshot.positions[right] ||
    snapshot.positions[left + 1] - snapshot.positions[right + 1] ||
    snapshot.positions[left + 2] - snapshot.positions[right + 2] ||
    snapshot.normals[left] - snapshot.normals[right] ||
    snapshot.normals[left + 1] - snapshot.normals[right + 1] ||
    snapshot.normals[left + 2] - snapshot.normals[right + 2] ||
    snapshot.confidence[rightIndex] - snapshot.confidence[leftIndex];
}

function boundedFeatureIndices(
  snapshot: ManualPlacementSurfaceSnapshot,
  indices: readonly number[],
): readonly number[] {
  const ordered = [...indices].sort((left, right) => compareFeatureIndices(snapshot, left, right));
  if (ordered.length <= MAX_FEATURE_EVIDENCE) return ordered;
  const sampled = new Array<number>(MAX_FEATURE_EVIDENCE);
  const stride = ordered.length / MAX_FEATURE_EVIDENCE;
  for (let index = 0; index < MAX_FEATURE_EVIDENCE; index += 1) {
    sampled[index] = ordered[Math.min(ordered.length - 1, Math.floor((index + 0.5) * stride))];
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
  for (const index of boundedFeatureIndices(snapshot, usable)) {
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
 * scans the surface. The bounded seed budget is distributed across coarse spatial
 * regions so a large flat/high-confidence area cannot starve a smaller true
 * feature elsewhere. Two spatially supported orientation families are sufficient
 * for this prefilter: requiring three here would miss real corners with a depth
 * hole at the vertex. The robust solver remains authoritative and still requires
 * THREE supported planes, bounded uncertainty and measured pair-edge support near
 * the solved intersection.
 *
 * This detector certifies geometry only. Candidate-specific temporal maturity is
 * handled by CertifiedStructuralCornerTracker; a single analysis is exactly one
 * physical observation and is never presented as multi-view persistence.
 */
export function detectCertifiedStructuralCorners(
  snapshot: ManualPlacementSurfaceSnapshot,
): readonly CertifiedCornerObservation[] {
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
    if (
      confidence > existing.confidence ||
      (confidence === existing.confidence && comparePoint(position, existing.position) < 0)
    ) {
      existing.position = position;
      existing.confidence = confidence;
    }
  }

  const seeds = spatiallyFairSeeds(
    [...cells.values()].filter((seed) => seed.count >= 2),
  );
  const spatialIndex = new BoundedSpatialPointIndex(snapshot.positions, count, FEATURE_INDEX_CELL_METERS);

  const corners: CertifiedCornerObservation[] = [];
  for (const seed of seeds) {
    if (!structuralFeatureNearSeed(snapshot, spatialIndex, seed)) continue;
    const result = solveManualPlacementOnStableSurface(seed.position, snapshot);
    const uncertaintyMeters = result.uncertaintyMeters ?? Number.POSITIVE_INFINITY;
    const minimumArmSpanMeters = result.minimumCornerArmSpanMeters ?? 0;
    if (
      !result.accepted ||
      result.mode !== 'corner' ||
      result.position === null ||
      result.confidence < MIN_CORNER_CONFIDENCE ||
      uncertaintyMeters > MAX_CORNER_UNCERTAINTY_METERS ||
      minimumArmSpanMeters <= 0
    ) continue;

    const candidate: CertifiedCornerObservation = {
      id: cornerId(result.position),
      kind: 'corner',
      position: result.position,
      confidence: result.confidence,
      geometryConfidence: result.confidence,
      support: result.supportCount,
      observations: 1,
      uncertaintyMeters,
      minimumArmSpanMeters,
    };
    const existing = corners.find((corner) =>
      distance(corner.position, candidate.position) <= certifiedCornerAssociationGateMeters(
        corner.uncertaintyMeters,
        candidate.uncertaintyMeters,
        corner.minimumArmSpanMeters,
        candidate.minimumArmSpanMeters,
      )
    );
    if (existing === undefined) {
      corners.push(candidate);
    } else if (
      candidate.confidence > existing.confidence ||
      (candidate.confidence === existing.confidence && comparePoint(candidate.position, existing.position) < 0)
    ) {
      const index = corners.indexOf(existing);
      corners[index] = candidate;
    }
  }

  return corners
    .sort((left, right) => right.confidence - left.confidence || comparePoint(left.position, right.position))
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
        const associationGate = certifiedCornerAssociationGateMeters(
          track.uncertaintyMeters,
          candidate.uncertaintyMeters,
          track.minimumArmSpanMeters,
          candidate.minimumArmSpanMeters,
        );
        if (candidateDistance > associationGate || candidateDistance >= bestDistance) continue;
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
          uncertaintyMeters: candidate.uncertaintyMeters,
          minimumArmSpanMeters: candidate.minimumArmSpanMeters,
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
      best.uncertaintyMeters = clamp(
        best.uncertaintyMeters * 0.65 + candidate.uncertaintyMeters * 0.35,
        0,
        MAX_CORNER_UNCERTAINTY_METERS,
      );
      best.minimumArmSpanMeters = Math.min(best.minimumArmSpanMeters, candidate.minimumArmSpanMeters);
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
      .sort((left, right) => right.confidence - left.confidence || comparePoint(left.position, right.position))
      .slice(0, MAX_CORNERS);
  }
}
