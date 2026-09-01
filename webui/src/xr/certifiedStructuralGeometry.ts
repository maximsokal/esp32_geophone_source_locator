import {
  solveManualPlacementOnStableSurface,
  type ManualPlacementSurfaceSnapshot,
  type ProjectPoint,
} from './manualPlacementSurface';
import type { StructuralCornerCandidate } from './structuralGuide';

const SEED_CELL_METERS = 0.13;
const MAX_SEEDS = 72;
const MAX_CORNERS = 8;
const MIN_CORNER_CONFIDENCE = 0.72;
const MAX_CORNER_UNCERTAINTY_METERS = 0.030;
const DEDUPE_METERS = 0.095;

interface Seed {
  position: ProjectPoint;
  confidence: number;
  count: number;
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

/**
 * Spatially sparse robust corner discovery on the already fused surface map.
 *
 * The legacy guide starts from many tiny planar patches. This complementary
 * detector starts from at most one strong seed per 13-cm cell, then asks the
 * uncertainty-aware local solver whether THREE supported planes really
 * intersect near measured data. It therefore cannot promote a 2-plane edge to
 * a corner merely because several patch normals happen to be available.
 */
export function detectCertifiedStructuralCorners(
  snapshot: ManualPlacementSurfaceSnapshot,
): readonly StructuralCornerCandidate[] {
  const count = Math.min(
    snapshot.pointCount,
    Math.floor(snapshot.positions.length / 3),
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

  const corners: StructuralCornerCandidate[] = [];
  for (const seed of seeds) {
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
      // The fused source already requires repeated observations; keep this
      // conservative and do not pretend to know exact tracker observation count.
      observations: 3,
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
