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
}

export interface ManualPlacementPreview {
  u: number;
  v: number;
  screenDistance: number | null;
  seedPosition: ProjectPoint | null;
  resolution: ManualPlacementResolution;
}

export type ManualPreviewProjector = (point: ProjectPoint) => ManualPreviewProjection | null;

const MIN_PREVIEW_CONFIDENCE = 0.58;
const DEFAULT_SCREEN_RADIUS = 0.065;

function rejected(message: string): ManualPlacementResolution {
  return {
    accepted: false,
    position: null,
    mode: null,
    confidence: 0,
    supportCount: 0,
    sourceDistanceMeters: null,
    message,
  };
}

/**
 * Resolve a cursor location against the already accumulated multi-view map.
 *
 * This intentionally does NOT read the current raw depth texel. A depth texel
 * is too noisy to decide where a structural corner is. We first find stable
 * promoted geometry that projects close to the cursor, then run the same
 * multi-plane solver used by final manual placement. If no stable geometry is
 * under the cursor, the UI must say SCAN MORE and placement stays disabled.
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
  let bestIndex = -1;
  let bestDistance = Number.POSITIVE_INFINITY;

  for (let index = 0; index < count; index += 1) {
    if (snapshot.confidence[index] < MIN_PREVIEW_CONFIDENCE) continue;
    const offset = index * 3;
    const point: ProjectPoint = [
      snapshot.positions[offset],
      snapshot.positions[offset + 1],
      snapshot.positions[offset + 2],
    ];
    const projected = project(point);
    if (projected === null || !projected.visible) continue;
    const distance = Math.hypot(projected.u - u, projected.v - v);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
    }
  }

  if (bestIndex < 0 || bestDistance > screenRadius) {
    return {
      u,
      v,
      screenDistance: Number.isFinite(bestDistance) ? bestDistance : null,
      seedPosition: null,
      resolution: rejected('SCAN MORE · no stable multi-view surface under the cursor.'),
    };
  }

  const offset = bestIndex * 3;
  const seedPosition: ProjectPoint = [
    snapshot.positions[offset],
    snapshot.positions[offset + 1],
    snapshot.positions[offset + 2],
  ];
  return {
    u,
    v,
    screenDistance: bestDistance,
    seedPosition,
    resolution: solveManualPlacementOnStableSurface(seedPosition, snapshot),
  };
}

export function manualPreviewLabel(preview: ManualPlacementPreview | null): string {
  if (preview === null) return 'SCAN MORE';
  const resolution = preview.resolution;
  if (!resolution.accepted || resolution.position === null || resolution.mode === null) return 'SCAN MORE';
  return resolution.mode === 'corner' ? 'CORNER LOCK' : 'EDGE LOCK';
}
