import type { PlanePolygonUv, StructuralPlanePrior } from './webxrPlaneGuidance';

const MIN_NORMAL_DOT = Math.cos((16 * Math.PI) / 180);
const MAX_PLANE_DISTANCE_METERS = 0.045;
const POLYGON_MARGIN_METERS = 0.04;
const POLYGON_MARGIN_SQUARED = POLYGON_MARGIN_METERS * POLYGON_MARGIN_METERS;
const MAX_NORMAL_BLEND = 0.20;
const MIN_SURFEL_CONFIDENCE = 0.50;

type Vec3 = readonly [number, number, number];

interface PreparedPlanePrior {
  center: Vec3;
  native: Vec3;
  tangentU: Vec3;
  tangentV: Vec3;
  polygonUv: readonly PlanePolygonUv[];
  extentRadiusMeters: number;
  minU: number;
  maxU: number;
  minV: number;
  maxV: number;
}

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function normalize(x: number, y: number, z: number): Vec3 | null {
  const magnitude = Math.hypot(x, y, z);
  return Number.isFinite(magnitude) && magnitude > 1e-8
    ? [x / magnitude, y / magnitude, z / magnitude]
    : null;
}
function pointSegmentDistanceSquared(u: number, v: number, a: PlanePolygonUv, b: PlanePolygonUv): number {
  const du = b[0] - a[0], dv = b[1] - a[1];
  const lengthSquared = du * du + dv * dv;
  if (lengthSquared <= 1e-12) {
    const eu = u - a[0], ev = v - a[1];
    return eu * eu + ev * ev;
  }
  const t = Math.max(0, Math.min(1, ((u - a[0]) * du + (v - a[1]) * dv) / lengthSquared));
  const eu = u - (a[0] + du * t), ev = v - (a[1] + dv * t);
  return eu * eu + ev * ev;
}
function insidePlanePolygonWithMargin(u: number, v: number, polygon: readonly PlanePolygonUv[]): boolean {
  if (polygon.length < 3) return false;
  let inside = false;
  for (let index = 0, previous = polygon.length - 1; index < polygon.length; previous = index++) {
    const a = polygon[previous], b = polygon[index];
    if (pointSegmentDistanceSquared(u, v, a, b) <= POLYGON_MARGIN_SQUARED) return true;
    const crosses = ((a[1] > v) !== (b[1] > v)) &&
      (u < (b[0] - a[0]) * (v - a[1]) / ((b[1] - a[1]) || Number.EPSILON) + a[0]);
    if (crosses) inside = !inside;
  }
  return inside;
}
function preparePlanePriors(priors: readonly StructuralPlanePrior[]): PreparedPlanePrior[] {
  const prepared: PreparedPlanePrior[] = [];
  for (const prior of priors) {
    const native = normalize(prior.normal[0], prior.normal[1], prior.normal[2]);
    const tangentU = normalize(prior.tangentU[0], prior.tangentU[1], prior.tangentU[2]);
    const tangentV = normalize(prior.tangentV[0], prior.tangentV[1], prior.tangentV[2]);
    if (
      native === null || tangentU === null || tangentV === null ||
      !prior.center.every(Number.isFinite) ||
      !Number.isFinite(prior.extentRadiusMeters) || prior.extentRadiusMeters <= 0 ||
      prior.polygonUv.length < 3
    ) continue;
    let minU = Number.POSITIVE_INFINITY, maxU = Number.NEGATIVE_INFINITY;
    let minV = Number.POSITIVE_INFINITY, maxV = Number.NEGATIVE_INFINITY;
    let valid = true;
    for (const point of prior.polygonUv) {
      if (!Number.isFinite(point[0]) || !Number.isFinite(point[1])) { valid = false; break; }
      minU = Math.min(minU, point[0]); maxU = Math.max(maxU, point[0]);
      minV = Math.min(minV, point[1]); maxV = Math.max(maxV, point[1]);
    }
    if (!valid || ![minU, maxU, minV, maxV].every(Number.isFinite)) continue;
    prepared.push({
      center: prior.center,
      native,
      tangentU,
      tangentV,
      polygonUv: prior.polygonUv,
      extentRadiusMeters: prior.extentRadiusMeters,
      minU,
      maxU,
      minV,
      maxV,
    });
  }
  return prepared;
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
  // Normalize plane basis and calculate cheap UV bounds once per worker analysis,
  // not once per surfel × plane. The exact polygon remains the final support gate.
  const prepared = preparePlanePriors(priors);
  if (prepared.length === 0) return 0;
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

    let bestNormal: Vec3 | null = null;
    let bestScore = 0;
    for (const prior of prepared) {
      const signedDot = measured[0] * prior.native[0] + measured[1] * prior.native[1] + measured[2] * prior.native[2];
      const similarity = Math.abs(signedDot);
      if (similarity < MIN_NORMAL_DOT) continue;

      const dx = px - prior.center[0], dy = py - prior.center[1], dz = pz - prior.center[2];
      const signedDistance = dx * prior.native[0] + dy * prior.native[1] + dz * prior.native[2];
      const planeDistance = Math.abs(signedDistance);
      if (planeDistance > MAX_PLANE_DISTANCE_METERS) continue;

      // Radius then UV AABB reject almost all unrelated surfel/plane pairs before
      // the exact polygon walk. This matters for thousands of measured surfels.
      const totalSquared = dx * dx + dy * dy + dz * dz;
      const tangentDistance = Math.sqrt(Math.max(0, totalSquared - planeDistance * planeDistance));
      if (tangentDistance > prior.extentRadiusMeters + POLYGON_MARGIN_METERS) continue;
      const localU = dx * prior.tangentU[0] + dy * prior.tangentU[1] + dz * prior.tangentU[2];
      const localV = dx * prior.tangentV[0] + dy * prior.tangentV[1] + dz * prior.tangentV[2];
      if (
        localU < prior.minU - POLYGON_MARGIN_METERS || localU > prior.maxU + POLYGON_MARGIN_METERS ||
        localV < prior.minV - POLYGON_MARGIN_METERS || localV > prior.maxV + POLYGON_MARGIN_METERS
      ) continue;
      if (!insidePlanePolygonWithMargin(localU, localV, prior.polygonUv)) continue;

      const normalScore = clamp01((similarity - MIN_NORMAL_DOT) / Math.max(1e-6, 1 - MIN_NORMAL_DOT));
      const distanceScore = clamp01(1 - planeDistance / MAX_PLANE_DISTANCE_METERS);
      const score = normalScore * 0.62 + distanceScore * 0.38;
      if (score > bestScore) {
        bestScore = score;
        bestNormal = signedDot < 0
          ? [-prior.native[0], -prior.native[1], -prior.native[2]]
          : prior.native;
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
