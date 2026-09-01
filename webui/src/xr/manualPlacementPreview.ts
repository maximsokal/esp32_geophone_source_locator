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

/**
 * Resolve a cursor location against the already accumulated multi-view map.
 *
 * Screen proximity alone is not a depth hit: foreground and background surfels
 * can project to almost the same pixel. We therefore first establish a narrow
 * screen-space ray cone and then choose the FRONTMOST stable surfel in that
 * cone using metric view depth. Only that visible surface seeds the robust
 * multi-plane solver. This prevents a floor/wall behind the intended object
 * from winning merely because its projected dot is one pixel closer.
 */
export function previewManualPlacementFromStableSurface(
  u: number,
  v: number,
  snapshot: ManualPlacementSurfaceSnapshot,
  project: ManualPreviewProjector,
  screenRadius = DEFAULT_SCREEN_RADIUS,
): ManualPlacementPreview {
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

  // Restrict depth competition to points genuinely close to the cursor ray.
  // Then choose the frontmost metric depth; fallback to closest screen point for
  // synthetic/unit projectors that do not expose view depth.
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

  const selected = nearRay[0];
  if (selected === undefined) {
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
