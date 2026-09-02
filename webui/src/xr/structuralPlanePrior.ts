import type { StructuralPlanePrior } from './webxrPlaneGuidance';

export type ProjectPoint = readonly [number, number, number];
export type ProjectVector = readonly [number, number, number];

export interface MeasuredPlanePatchLike {
  center: ProjectPoint;
  normal: ProjectVector;
  confidence: number;
  planarity: number;
}

const MAX_PRIOR_NORMAL_ANGLE_RADIANS = (18 * Math.PI) / 180;
const MIN_PRIOR_NORMAL_DOT = Math.cos(MAX_PRIOR_NORMAL_ANGLE_RADIANS);
const MAX_PRIOR_PLANE_DISTANCE_METERS = 0.055;
const PRIOR_EXTENT_MARGIN_METERS = 0.16;
const MAX_PRIOR_BLEND = 0.34;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function dot(a: ProjectVector, b: ProjectVector): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
function normalize(vector: ProjectVector): ProjectVector | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return Number.isFinite(magnitude) && magnitude > 1e-8
    ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude]
    : null;
}

interface PriorMatch { prior: StructuralPlanePrior; score: number; alignedNormal: ProjectVector; }

function matchPrior(
  measured: MeasuredPlanePatchLike,
  priors: readonly StructuralPlanePrior[],
): PriorMatch | null {
  const measuredNormal = normalize(measured.normal);
  if (measuredNormal === null) return null;
  let best: PriorMatch | null = null;
  for (const prior of priors) {
    const priorNormal = normalize(prior.normal);
    if (priorNormal === null || prior.extentRadiusMeters <= 0) continue;
    const signedDot = dot(measuredNormal, priorNormal);
    const similarity = Math.abs(signedDot);
    if (similarity < MIN_PRIOR_NORMAL_DOT) continue;
    const dx = measured.center[0] - prior.center[0];
    const dy = measured.center[1] - prior.center[1];
    const dz = measured.center[2] - prior.center[2];
    const planeDistance = Math.abs(dx * priorNormal[0] + dy * priorNormal[1] + dz * priorNormal[2]);
    if (planeDistance > MAX_PRIOR_PLANE_DISTANCE_METERS) continue;
    const totalSquared = dx * dx + dy * dy + dz * dz;
    const tangentDistance = Math.sqrt(Math.max(0, totalSquared - planeDistance * planeDistance));
    if (tangentDistance > prior.extentRadiusMeters + PRIOR_EXTENT_MARGIN_METERS) continue;

    const normalScore = clamp01(
      (similarity - MIN_PRIOR_NORMAL_DOT) / Math.max(1e-6, 1 - MIN_PRIOR_NORMAL_DOT),
    );
    const distanceScore = clamp01(1 - planeDistance / MAX_PRIOR_PLANE_DISTANCE_METERS);
    const extentScore = clamp01(1 - tangentDistance / Math.max(0.08, prior.extentRadiusMeters + PRIOR_EXTENT_MARGIN_METERS));
    // The WebXR API exposes no numeric plane confidence. Area/semantics therefore
    // never become confidence; only geometric agreement determines this soft score.
    const score = normalScore * 0.45 + distanceScore * 0.35 + extentScore * 0.20;
    if (best === null || score > best.score) {
      best = {
        prior,
        score,
        alignedNormal: signedDot < 0
          ? [-priorNormal[0], -priorNormal[1], -priorNormal[2]]
          : priorNormal,
      };
    }
  }
  return best;
}

/**
 * Softly stabilize an ALREADY measured plane patch with a matching native XRPlane.
 *
 * This function cannot create a patch, move its center onto an unseen native
 * polygon, or mark any volume as free/occupied. The physical surfel fit remains
 * authoritative; a nearby native plane only reduces normal jitter and gives a
 * small bounded confidence/planarity bonus.
 */
export function stabilizeMeasuredPlaneWithNativePrior<T extends MeasuredPlanePatchLike>(
  measured: T,
  priors: readonly StructuralPlanePrior[] | undefined,
): T {
  if (priors === undefined || priors.length === 0) return measured;
  const match = matchPrior(measured, priors);
  if (match === null) return measured;
  const measuredNormal = normalize(measured.normal);
  if (measuredNormal === null) return measured;
  const blend = Math.min(MAX_PRIOR_BLEND, 0.16 + match.score * 0.18);
  const normal = normalize([
    measuredNormal[0] * (1 - blend) + match.alignedNormal[0] * blend,
    measuredNormal[1] * (1 - blend) + match.alignedNormal[1] * blend,
    measuredNormal[2] * (1 - blend) + match.alignedNormal[2] * blend,
  ]);
  if (normal === null) return measured;
  return {
    ...measured,
    normal,
    confidence: clamp01(measured.confidence + 0.055 * match.score),
    planarity: clamp01(measured.planarity + 0.045 * match.score),
  };
}
