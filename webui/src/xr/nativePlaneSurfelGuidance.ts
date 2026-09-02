import type { StructuralPlanePrior } from './webxrPlaneGuidance';

const MIN_NORMAL_DOT = Math.cos((16 * Math.PI) / 180);
const MAX_PLANE_DISTANCE_METERS = 0.045;
const EXTENT_MARGIN_METERS = 0.12;
const MAX_NORMAL_BLEND = 0.20;
const MIN_SURFEL_CONFIDENCE = 0.50;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function normalize(x: number, y: number, z: number): readonly [number, number, number] | null {
  const magnitude = Math.hypot(x, y, z);
  return Number.isFinite(magnitude) && magnitude > 1e-8
    ? [x / magnitude, y / magnitude, z / magnitude]
    : null;
}

/**
 * Softly stabilize ALREADY MEASURED surfel normals with matching native XRPlane
 * orientation. Positions and physical confidence are immutable and no surfel is
 * created or removed.
 *
 * The arrays are intentionally modified in-place: structural worker input is
 * transferred from the main thread and is worker-owned at this point, avoiding
 * another full point-cloud allocation. WebXR Plane Detection exposes no numeric
 * plane confidence, so the prior is never allowed to promote surfel confidence.
 */
export function stabilizeMeasuredSurfelsWithNativePlanes(
  positions: Float32Array,
  normals: Float32Array,
  confidence: Float32Array,
  priors: readonly StructuralPlanePrior[] | undefined,
): number {
  if (priors === undefined || priors.length === 0) return 0;
  const count = Math.min(
    Math.floor(positions.length / 3),
    Math.floor(normals.length / 3),
    confidence.length,
  );
  let stabilized = 0;

  for (let index = 0; index < count; index += 1) {
    if (!Number.isFinite(confidence[index]) || confidence[index] < MIN_SURFEL_CONFIDENCE) continue;
    const offset = index * 3;
    const px = positions[offset], py = positions[offset + 1], pz = positions[offset + 2];
    if (![px, py, pz].every(Number.isFinite)) continue;
    const measured = normalize(normals[offset], normals[offset + 1], normals[offset + 2]);
    if (measured === null) continue;

    let bestNormal: readonly [number, number, number] | null = null;
    let bestScore = 0;
    for (const prior of priors) {
      const native = normalize(prior.normal[0], prior.normal[1], prior.normal[2]);
      if (native === null || !Number.isFinite(prior.extentRadiusMeters) || prior.extentRadiusMeters <= 0) continue;
      const signedDot = measured[0] * native[0] + measured[1] * native[1] + measured[2] * native[2];
      const similarity = Math.abs(signedDot);
      if (similarity < MIN_NORMAL_DOT) continue;

      const dx = px - prior.center[0], dy = py - prior.center[1], dz = pz - prior.center[2];
      const signedDistance = dx * native[0] + dy * native[1] + dz * native[2];
      const planeDistance = Math.abs(signedDistance);
      if (planeDistance > MAX_PLANE_DISTANCE_METERS) continue;
      const totalSquared = dx * dx + dy * dy + dz * dz;
      const tangentDistance = Math.sqrt(Math.max(0, totalSquared - planeDistance * planeDistance));
      if (tangentDistance > prior.extentRadiusMeters + EXTENT_MARGIN_METERS) continue;

      const normalScore = clamp01((similarity - MIN_NORMAL_DOT) / Math.max(1e-6, 1 - MIN_NORMAL_DOT));
      const distanceScore = clamp01(1 - planeDistance / MAX_PLANE_DISTANCE_METERS);
      const score = normalScore * 0.62 + distanceScore * 0.38;
      if (score > bestScore) {
        bestScore = score;
        bestNormal = signedDot < 0 ? [-native[0], -native[1], -native[2]] : native;
      }
    }

    if (bestNormal === null) continue;
    const blend = Math.min(MAX_NORMAL_BLEND, 0.08 + bestScore * 0.12);
    const blended = normalize(
      measured[0] * (1 - blend) + bestNormal[0] * blend,
      measured[1] * (1 - blend) + bestNormal[1] * blend,
      measured[2] * (1 - blend) + bestNormal[2] * blend,
    );
    if (blended === null) continue;
    normals[offset] = blended[0];
    normals[offset + 1] = blended[1];
    normals[offset + 2] = blended[2];
    stabilized += 1;
  }
  return stabilized;
}
