/**
 * The saved scan A as a reference drawn over the live camera of a Continue session B, once B is tied to A by known tags (issue #45, gate 2).
 *
 * A's points are in A's project frame (Z up, metres). The lock gives T_BA (yaw about +Z, then a translation: B -> A, project frame), so a point of A is
 * brought into B by the inverse, and then from the project frame into the WebXR local-floor frame (Y up) that the XR layer renders in.
 * The overlay is a view of MEASURED points only; it adds nothing to the scan and nothing is written anywhere.
 */

import type { RelockTransform } from './continuationRelocalization';

export interface ReferenceOverlay {
  /** WebXR local-floor positions of the current session, 3 floats per point. */
  positions: Float32Array;
  /** One value per point in [0,1]; constant here: the overlay shows where A was measured, not how well. */
  confidence: Float32Array;
  pointCount: number;
}

/** Points the overlay draws at most: a faint reference, not a second scan to render at full cost. */
export const REFERENCE_OVERLAY_MAX_POINTS = 6000;
/** Confidence given to every overlay point (the existing point shader colours by it). */
export const REFERENCE_OVERLAY_CONFIDENCE = 0.5;

/** A -> B in project metres, then project -> WebXR local-floor (x, z, -y). Exposed for tests. */
export function baseToExtensionLocalFloor(point: readonly [number, number, number], transform: RelockTransform): [number, number, number] {
  const x = point[0] - transform.tx;
  const y = point[1] - transform.ty;
  const z = point[2] - transform.tz;
  const c = Math.cos(-transform.yaw);
  const s = Math.sin(-transform.yaw);
  const bx = c * x - s * y;
  const by = s * x + c * y;
  return [bx, z, -by];
}

/**
 * Even decimation of A's measured points into the frame of B. `positionsProject` is the saved scan's stable point list (project frame); non-finite
 * triples are skipped, so a damaged sample never reaches the GPU. Null for an unusable transform: nothing is drawn rather than something misplaced.
 */
export function buildReferenceOverlay(
  positionsProject: Float32Array,
  transform: RelockTransform | null,
  maxPoints = REFERENCE_OVERLAY_MAX_POINTS,
): ReferenceOverlay | null {
  if (transform === null || ![transform.yaw, transform.tx, transform.ty, transform.tz].every(Number.isFinite)) return null;
  const available = Math.floor(positionsProject.length / 3);
  if (available <= 0 || !(maxPoints > 0)) return null;
  const stride = Math.max(1, Math.ceil(available / maxPoints));
  const positions = new Float32Array(Math.min(available, maxPoints) * 3);
  let count = 0;
  for (let index = 0; index < available && count < maxPoints; index += stride) {
    const x = positionsProject[index * 3];
    const y = positionsProject[index * 3 + 1];
    const z = positionsProject[index * 3 + 2];
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    const local = baseToExtensionLocalFloor([x, y, z], transform);
    positions[count * 3] = local[0];
    positions[count * 3 + 1] = local[1];
    positions[count * 3 + 2] = local[2];
    count += 1;
  }
  if (count === 0) return null;
  return { positions: positions.slice(0, count * 3), confidence: new Float32Array(count).fill(REFERENCE_OVERLAY_CONFIDENCE), pointCount: count };
}
