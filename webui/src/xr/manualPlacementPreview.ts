import {
  getCertifiedGuideCorners,
  isCertifiedGuideRegistryActive,
} from './certifiedCornerRegistry';
import {
  solveManualPlacementOnStableSurface,
  type ManualPlacementResolution,
  type ManualPlacementSurfaceSnapshot,
  type ProjectPoint,
} from './manualPlacementSurface';

export interface ManualPreviewProjection {
  u: number;
  v: number;
  visible: boolean;
  /** Positive metric camera-space depth when the runtime projection can provide it. */
  viewDepthMeters?: number;
}

export interface ManualPlacementPreview {
  u: number;
  v: number;
  screenDistance: number | null;
  seedPosition: ProjectPoint | null;
  seedViewDepthMeters?: number | null;
  resolution: ManualPlacementResolution;
}

export type ManualPreviewProjector = (point: ProjectPoint) => ManualPreviewProjection | null;

const MIN_PREVIEW_CONFIDENCE = 0.62;
const DEFAULT_SCREEN_RADIUS = 0.045;
const FRONTMOST_SCREEN_BAND = 0.018;

function rejected(message: string): ManualPlacementResolution {
  return {
    accepted: false,
    position: null,
    mode: null,
    confidence: 0,
    supportCount: 0,
    sourceDistanceMeters: null,
    message,
    uncertaintyMeters: null,
    conditionNumber: null,
  };
}

interface ProjectedCandidate {
  index: number;
  screenDistance: number;
  viewDepthMeters: number | null;
}

function frontmostCandidate<T extends { screenDistance: number; viewDepthMeters: number | null }>(
  candidates: readonly T[],
  nearestScreenDistance: number,
  screenRadius: number,
): T | null {
  const screenBand = Math.min(screenRadius, Math.max(0.012, nearestScreenDistance + FRONTMOST_SCREEN_BAND));
  const nearRay = candidates.filter((candidate) => candidate.screenDistance <= screenBand);
  nearRay.sort((left, right) => {
    if (left.viewDepthMeters !== null && right.viewDepthMeters !== null) {
      const depthDifference = left.viewDepthMeters - right.viewDepthMeters;
      if (Math.abs(depthDifference) > 0.008) return depthDifference;
    } else if (left.viewDepthMeters !== null) {
      return -1;
    } else if (right.viewDepthMeters !== null) {
      return 1;
    }
    return left.screenDistance - right.screenDistance;
  });
  return nearRay[0] ?? null;
}

/**
 * In an active XR scan manual placement consumes the SAME robust state machine
 * as AUTO GUIDE. If no certified three-plane corner / multi-view finite edge
 * endpoint exists, the cursor cannot invent a weaker private hypothesis.
 *
 * The legacy stable-surface path remains only for isolated unit/offline callers
 * where the guide registry is intentionally inactive.
 */
function previewCertifiedCorner(
  u: number,
  v: number,
  project: ManualPreviewProjector,
  screenRadius: number,
): ManualPlacementPreview | null {
  if (!isCertifiedGuideRegistryActive()) return null;
  const registered = getCertifiedGuideCorners();
  let nearestScreenDistance = Number.POSITIVE_INFINITY;
  const projected: Array<{
    candidateIndex: number;
    screenDistance: number;
    viewDepthMeters: number | null;
  }> = [];

  registered.forEach((candidate, candidateIndex) => {
    const screen = project(candidate.position);
    if (screen === null || !screen.visible) return;
    const screenDistance = Math.hypot(screen.u - u, screen.v - v);
    nearestScreenDistance = Math.min(nearestScreenDistance, screenDistance);
    if (screenDistance > screenRadius) return;
    const depth = Number(screen.viewDepthMeters);
    projected.push({
      candidateIndex,
      screenDistance,
      viewDepthMeters: Number.isFinite(depth) && depth > 0 ? depth : null,
    });
  });

  if (projected.length === 0) {
    return {
      u,
      v,
      screenDistance: Number.isFinite(nearestScreenDistance) ? nearestScreenDistance : null,
      seedPosition: null,
      seedViewDepthMeters: null,
      resolution: rejected(
        registered.length === 0
          ? 'SCAN MORE · no corner in the scene has passed the robust geometry certificate yet.'
          : 'SCAN MORE · move the reticle onto a certified AUTO GUIDE corner.',
      ),
    };
  }

  const selected = frontmostCandidate(projected, nearestScreenDistance, screenRadius);
  if (selected === null) {
    return {
      u,
      v,
      screenDistance: nearestScreenDistance,
      seedPosition: null,
      seedViewDepthMeters: null,
      resolution: rejected('SCAN MORE · no frontmost certified corner could be resolved.'),
    };
  }
  const candidate = registered[selected.candidateIndex];
  return {
    u,
    v,
    screenDistance: selected.screenDistance,
    seedPosition: candidate.position,
    seedViewDepthMeters: selected.viewDepthMeters,
    resolution: {
      accepted: true,
      position: candidate.position,
      mode: 'corner',
      confidence: candidate.confidence,
      supportCount: candidate.support,
      sourceDistanceMeters: 0,
      message: candidate.source === 'multi-view-edge-endpoint'
        ? 'CORNER LOCK · certified finite edge endpoint from independent views.'
        : 'CORNER LOCK · certified robust three-plane intersection.',
      uncertaintyMeters: null,
      conditionNumber: null,
    },
  };
}

/**
 * Resolve a cursor location against accumulated multi-view geometry.
 *
 * Production XR uses the shared certified-corner registry above. The fallback
 * below keeps the lower-level solver testable without a mounted guide overlay.
 */
export function previewManualPlacementFromStableSurface(
  u: number,
  v: number,
  snapshot: ManualPlacementSurfaceSnapshot,
  project: ManualPreviewProjector,
  screenRadius = DEFAULT_SCREEN_RADIUS,
): ManualPlacementPreview {
  const certified = previewCertifiedCorner(u, v, project, screenRadius);
  if (certified !== null) return certified;

  const count = Math.min(
    snapshot.pointCount,
    Math.floor(snapshot.positions.length / 3),
    snapshot.confidence.length,
  );
  const candidates: ProjectedCandidate[] = [];
  let nearestScreenDistance = Number.POSITIVE_INFINITY;

  for (let index = 0; index < count; index += 1) {
    if (snapshot.confidence[index] < MIN_PREVIEW_CONFIDENCE) continue;
    if (snapshot.coverageReady !== undefined && index < snapshot.coverageReady.length && snapshot.coverageReady[index] === 0) continue;
    const offset = index * 3;
    const point: ProjectPoint = [
      snapshot.positions[offset],
      snapshot.positions[offset + 1],
      snapshot.positions[offset + 2],
    ];
    const projected = project(point);
    if (projected === null || !projected.visible) continue;
    const screenDistance = Math.hypot(projected.u - u, projected.v - v);
    nearestScreenDistance = Math.min(nearestScreenDistance, screenDistance);
    if (screenDistance > screenRadius) continue;
    const depth = Number(projected.viewDepthMeters);
    candidates.push({
      index,
      screenDistance,
      viewDepthMeters: Number.isFinite(depth) && depth > 0 ? depth : null,
    });
  }

  if (candidates.length === 0 || nearestScreenDistance > screenRadius) {
    return {
      u,
      v,
      screenDistance: Number.isFinite(nearestScreenDistance) ? nearestScreenDistance : null,
      seedPosition: null,
      seedViewDepthMeters: null,
      resolution: rejected('SCAN MORE · no stable multi-view surface under the cursor.'),
    };
  }

  const selected = frontmostCandidate(candidates, nearestScreenDistance, screenRadius);
  if (selected === null) {
    return {
      u,
      v,
      screenDistance: nearestScreenDistance,
      seedPosition: null,
      seedViewDepthMeters: null,
      resolution: rejected('SCAN MORE · no frontmost stable surface could be resolved.'),
    };
  }

  const offset = selected.index * 3;
  const seedPosition: ProjectPoint = [
    snapshot.positions[offset],
    snapshot.positions[offset + 1],
    snapshot.positions[offset + 2],
  ];
  return {
    u,
    v,
    screenDistance: selected.screenDistance,
    seedPosition,
    seedViewDepthMeters: selected.viewDepthMeters,
    resolution: solveManualPlacementOnStableSurface(seedPosition, snapshot),
  };
}

export function manualPreviewLabel(preview: ManualPlacementPreview | null): string {
  if (preview === null) return 'SCAN MORE';
  const resolution = preview.resolution;
  if (!resolution.accepted || resolution.position === null || resolution.mode === null) return 'SCAN MORE';
  return resolution.mode === 'corner' ? 'CORNER LOCK' : 'EDGE LOCK';
}
