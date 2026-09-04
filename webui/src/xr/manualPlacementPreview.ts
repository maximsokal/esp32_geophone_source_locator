import {
  getCertifiedGuideCorners,
  getCertifiedGuideEdges,
  getCertifiedGuideRegistrySummary,
} from './certifiedCornerRegistry';
import { getLatestProjectRay } from './latestProjectionRegistry';
import { intersectCameraRayWithCertifiedEdge } from './manualRayEdgePlacement';
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
    accepted: false, position: null, mode: null, confidence: 0, supportCount: 0,
    sourceDistanceMeters: null, message, uncertaintyMeters: null, conditionNumber: null,
  };
}

interface ProjectedCandidate { index: number; screenDistance: number; viewDepthMeters: number | null; }

function frontmostCandidate<T extends { screenDistance: number; viewDepthMeters: number | null }>(
  candidates: readonly T[], nearestScreenDistance: number, screenRadius: number,
): T | null {
  const screenBand = Math.min(screenRadius, Math.max(0.012, nearestScreenDistance + FRONTMOST_SCREEN_BAND));
  const nearRay = candidates.filter((candidate) => candidate.screenDistance <= screenBand);
  nearRay.sort((left, right) => {
    if (left.viewDepthMeters !== null && right.viewDepthMeters !== null) {
      const depthDifference = left.viewDepthMeters - right.viewDepthMeters;
      if (Math.abs(depthDifference) > 0.008) return depthDifference;
    } else if (left.viewDepthMeters !== null) return -1;
    else if (right.viewDepthMeters !== null) return 1;
    return left.screenDistance - right.screenDistance;
  });
  return nearRay[0] ?? null;
}

function previewCertifiedGeometry(
  u: number,
  v: number,
  project: ManualPreviewProjector,
  screenRadius: number,
): ManualPlacementPreview | null {
  const registry = getCertifiedGuideRegistrySummary();
  /*
   * An active-but-empty worker registry means "no globally certified target yet",
   * not "the local physical surface resolver is forbidden". Returning null here
   * hands control to solveManualPlacementOnStableSurface(), which still requires
   * robust multi-plane measured support and applies the exact ray-verified FREE
   * veto. Once any certified target exists we keep the stricter registry path.
   */
  if (!registry.active || !registry.hasTargets) return null;
  const registered = getCertifiedGuideCorners();
  let nearestScreenDistance = Number.POSITIVE_INFINITY;
  const projected: Array<{ candidateIndex: number; screenDistance: number; viewDepthMeters: number | null }> = [];

  registered.forEach((candidate, candidateIndex) => {
    const screen = project(candidate.position);
    if (screen === null || !screen.visible) return;
    const screenDistance = Math.hypot(screen.u - u, screen.v - v);
    nearestScreenDistance = Math.min(nearestScreenDistance, screenDistance);
    if (screenDistance > screenRadius) return;
    const depth = Number(screen.viewDepthMeters);
    projected.push({ candidateIndex, screenDistance, viewDepthMeters: Number.isFinite(depth) && depth > 0 ? depth : null });
  });

  if (projected.length > 0) {
    const selected = frontmostCandidate(projected, nearestScreenDistance, screenRadius);
    if (selected !== null) {
      const candidate = registered[selected.candidateIndex];
      const message = candidate.source === 'multi-view-edge-endpoint'
        ? 'CORNER LOCK · certified finite edge endpoint from independent views.'
        : candidate.source === 'multi-edge-vertex'
          ? 'CORNER LOCK · vertex where two mature finite 3D edges converge.'
          : 'CORNER LOCK · certified robust three-plane intersection.';
      return {
        u, v, screenDistance: selected.screenDistance, seedPosition: candidate.position,
        seedViewDepthMeters: selected.viewDepthMeters,
        resolution: {
          accepted: true, position: candidate.position, mode: 'corner', confidence: candidate.confidence,
          supportCount: candidate.support, sourceDistanceMeters: 0,
          message,
          uncertaintyMeters: null, conditionNumber: null,
        },
      };
    }
  }

  // Two robust planes already define a metric 3D edge. The user's reticle ray
  // supplies the one missing degree of freedom along that line; no depth sample
  // at the physical corner/edge pixel is required.
  const ray = getLatestProjectRay(u, v);
  if (ray !== null) {
    let best: ReturnType<typeof intersectCameraRayWithCertifiedEdge> | null = null;
    let bestEdge = null as ReturnType<typeof getCertifiedGuideEdges>[number] | null;
    for (const edge of getCertifiedGuideEdges()) {
      const solution = intersectCameraRayWithCertifiedEdge(ray, edge);
      if (!solution.accepted || solution.position === null) continue;
      if (best === null || solution.confidence > best.confidence ||
        (solution.confidence === best.confidence && (solution.skewDistanceMeters ?? Infinity) < (best.skewDistanceMeters ?? Infinity))) {
        best = solution; bestEdge = edge;
      }
    }
    if (best !== null && best.position !== null && bestEdge !== null) {
      const projectedEdgePoint = project(best.position);
      return {
        u, v,
        screenDistance: projectedEdgePoint === null ? 0 : Math.hypot(projectedEdgePoint.u - u, projectedEdgePoint.v - v),
        seedPosition: best.position,
        seedViewDepthMeters: projectedEdgePoint?.viewDepthMeters ?? null,
        resolution: {
          accepted: true, position: best.position, mode: 'edge', confidence: best.confidence,
          supportCount: bestEdge.support, sourceDistanceMeters: best.skewDistanceMeters,
          message: best.message, uncertaintyMeters: best.skewDistanceMeters, conditionNumber: best.condition,
        },
      };
    }
  }

  return {
    u, v,
    screenDistance: Number.isFinite(nearestScreenDistance) ? nearestScreenDistance : null,
    seedPosition: null, seedViewDepthMeters: null,
    resolution: rejected('SCAN MORE · aim at a certified edge/corner or collect another viewpoint.'),
  };
}

export function previewManualPlacementFromStableSurface(
  u: number,
  v: number,
  snapshot: ManualPlacementSurfaceSnapshot,
  project: ManualPreviewProjector,
  screenRadius = DEFAULT_SCREEN_RADIUS,
): ManualPlacementPreview {
  const certified = previewCertifiedGeometry(u, v, project, screenRadius);
  if (certified !== null) return certified;

  const count = Math.min(snapshot.pointCount, Math.floor(snapshot.positions.length / 3), snapshot.confidence.length);
  const candidates: ProjectedCandidate[] = [];
  let nearestScreenDistance = Number.POSITIVE_INFINITY;
  for (let index = 0; index < count; index += 1) {
    if (snapshot.confidence[index] < MIN_PREVIEW_CONFIDENCE) continue;
    if (snapshot.coverageReady !== undefined && index < snapshot.coverageReady.length && snapshot.coverageReady[index] === 0) continue;
    const offset = index * 3;
    const point: ProjectPoint = [snapshot.positions[offset], snapshot.positions[offset + 1], snapshot.positions[offset + 2]];
    const projected = project(point);
    if (projected === null || !projected.visible) continue;
    const screenDistance = Math.hypot(projected.u - u, projected.v - v);
    nearestScreenDistance = Math.min(nearestScreenDistance, screenDistance);
    if (screenDistance > screenRadius) continue;
    const depth = Number(projected.viewDepthMeters);
    candidates.push({ index, screenDistance, viewDepthMeters: Number.isFinite(depth) && depth > 0 ? depth : null });
  }

  if (candidates.length === 0 || nearestScreenDistance > screenRadius) {
    return { u, v, screenDistance: Number.isFinite(nearestScreenDistance) ? nearestScreenDistance : null, seedPosition: null, seedViewDepthMeters: null, resolution: rejected('SCAN MORE · no stable multi-view surface under the cursor.') };
  }
  const selected = frontmostCandidate(candidates, nearestScreenDistance, screenRadius);
  if (selected === null) {
    return { u, v, screenDistance: nearestScreenDistance, seedPosition: null, seedViewDepthMeters: null, resolution: rejected('SCAN MORE · no frontmost stable surface could be resolved.') };
  }
  const offset = selected.index * 3;
  const seedPosition: ProjectPoint = [snapshot.positions[offset], snapshot.positions[offset + 1], snapshot.positions[offset + 2]];
  return {
    u, v, screenDistance: selected.screenDistance, seedPosition, seedViewDepthMeters: selected.viewDepthMeters,
    resolution: solveManualPlacementOnStableSurface(seedPosition, snapshot),
  };
}

export function manualPreviewLabel(preview: ManualPlacementPreview | null): string {
  if (preview === null) return 'SCAN MORE';
  const resolution = preview.resolution;
  if (!resolution.accepted || resolution.position === null || resolution.mode === null) return 'SCAN MORE';
  return resolution.mode === 'corner' ? 'CORNER LOCK' : 'EDGE LOCK';
}
