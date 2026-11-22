/**
 * Tag-locked re-localisation of a Continue scan (issue #45, gate 1).
 *
 * Continue starts a new WebXR session B. Its frame is not the saved scan's frame A, so before any depth of B is allowed into A the phone must
 * prove where B is relative to A. The proof is the AprilTags that were confirmed in A and are seen again now:
 *
 *  - only DISTINCT tag ids known in A count (one tag seen eight times is one tag), each seen reliably in B: several sightings from at least two
 *    views, never a frame jump (`FusedTagObservation.frameJump`), never a tag A does not know or did not confirm;
 *  - three correspondences constrain a rigid transform but do not protect against a wrong one, so the geometry is checked as well: the tags must
 *    not lie on a line, and every pair must be as far apart in B as it is in A (a moved or swapped tag shows up here);
 *  - the transform is yaw about the vertical plus a translation. Both frames are gravity aligned (WebXR local-floor and the project frame), so
 *    no tilt and, by construction, no reflection and no scale. The fit is closed form, so it does not depend on the order of arrival;
 *  - when a fourth tag is available it is one more witness: a tag that disagrees with the others is dropped instead of dragging the fit.
 *
 * Pure: no WebXR, no React, no I/O. The caller owns the reference-space epoch and calls `evaluateRelock` with the observations of ONE epoch.
 */

import type { FusedTagObservation, TagObservation, TagObservationFusion } from './tagObservationFusion';
import { triangleHeightMm } from './tagLayout';
import type { TagPositionsArtifact } from './tagPositionsArtifact';
import { xrToProject } from './liveTagTracker';

/** How many distinct, known, confirmed tags the lock needs. */
export const RELOCK_MIN_TAGS = 3;
/** A tag must be seen at least this often in B. One view is enough: the lock is cross-checked by the tags' mutual distances against A (15 mm tolerance) and by an RMS gate, and the 2026-11-22 run showed operators glance at each tag from one place. */
export const RELOCK_MIN_OBSERVATIONS = 3;
export const RELOCK_MIN_VIEWS = 1;
/** The best triangle of the tags must have at least this height (same as the saved layout's gate): nearer to a line cannot fix a rotation. */
export const RELOCK_MIN_TRIANGLE_HEIGHT_MM = 30;
/** A pair of tags may be this much farther or nearer in B than in A, or three sigma of the four positions when that is larger. */
export const RELOCK_MIN_DISTANCE_TOLERANCE_MM = 15;
/** The fitted tags must agree with the transform within this RMS. */
export const RELOCK_MAX_RMS_MM = 20;

export type RelockReason =
  | 'need_more_tags'
  | 'collinear'
  | 'distance_mismatch'
  | 'residual_too_high';

export interface RelockBaseTag {
  tagId: number;
  /** The tag's position in the saved scan A, project frame (Z up), millimetres. */
  positionMm: readonly [number, number, number];
  /** 1-sigma position uncertainty in A, millimetres. */
  uncertaintyMm: number;
}

/**
 * How far back, per tag, a sighting still counts for the lock. Tracking drifts (phone run 2026-11-22: one tag seen 80 s apart was 19 cm apart in the session's
 * own frame, which pushed the RMS of an otherwise exact lock from 10 mm to over 20 mm and cost the lock two seconds after it was found). Each tag is judged by
 * its latest sightings only.
 */
export const RELOCK_TAG_RECENCY_MS = 15000;

/** Keeps, for each tag, the sightings within {@link RELOCK_TAG_RECENCY_MS} of that tag's newest one. */
export function recentTagObservations(observations: readonly TagObservation[], recencyMs = RELOCK_TAG_RECENCY_MS): TagObservation[] {
  const newest = new Map<number, number>();
  for (const observation of observations) {
    if (observation.capturedAtMs === undefined || !Number.isFinite(observation.capturedAtMs)) continue;
    newest.set(observation.tagId, Math.max(newest.get(observation.tagId) ?? Number.NEGATIVE_INFINITY, observation.capturedAtMs));
  }
  return observations.filter((observation) => {
    const latest = newest.get(observation.tagId);
    return latest === undefined || observation.capturedAtMs === undefined || !Number.isFinite(observation.capturedAtMs) || observation.capturedAtMs >= latest - recencyMs;
  });
}

/** The yaw + translation that maps B into A in the project frame (Z up): metres, the convention of the continuation transform. */
export interface RelockTransform { yaw: number; tx: number; ty: number; tz: number }

export interface RelockState {
  phase: 'relocalizing' | 'locked';
  /** Distinct tags that count toward the lock right now (0..needed and above). */
  found: number;
  needed: number;
  foundTagIds: number[];
  reason: RelockReason | null;
  transform: RelockTransform | null;
  /** RMS of the fitted tags, millimetres, once a fit exists. */
  rmsMm: number | null;
  /** The tags the transform was fitted to; a tag dropped as inconsistent is not listed. */
  usedTagIds: number[];
}

export const INITIAL_RELOCK_STATE: RelockState = Object.freeze({
  phase: 'relocalizing', found: 0, needed: RELOCK_MIN_TAGS, foundTagIds: [], reason: 'need_more_tags', transform: null, rmsMm: null, usedTagIds: [],
}) as RelockState;

/**
 * The tags saved with scan A that may anchor a re-lock: the RELIABLE ones (confirmed, or seen often enough from enough views), in A's project frame. The artifact's layout is relative to MAIN and
 * axis-aligned with the scan, so a tag's position in A is MAIN's scan position plus its layout offset.
 */
export function relockBaseTags(artifact: TagPositionsArtifact | null, expectedScanId: string | null): RelockBaseTag[] {
  if (artifact === null || artifact.scan_frame_id === null || artifact.scan_frame_id !== expectedScanId) return [];
  const [mx, my, mz] = artifact.main_scan_position_mm;
  if (![mx, my, mz].every(Number.isFinite)) return [];
  const seen = new Set<number>();
  const tags: RelockBaseTag[] = [];
  for (const node of artifact.nodes) {
    /* Reliable in A by the same measure as in B: confirmed, or at least seen several times from several views. Real scans seldom confirm more than one
       tag (the phone run of 2026-11-22 saved 4 usable tags, only one of them 'confirmed'), and refusing the rest left a Continue with no lock at all. */
    const reliable = node.confirmed || (node.observations >= RELOCK_MIN_OBSERVATIONS && node.views >= RELOCK_MIN_VIEWS);
    if (!reliable || seen.has(node.tag_id)) continue;
    if (![node.x_mm, node.y_mm, node.z_mm].every(Number.isFinite)) continue;
    seen.add(node.tag_id);
    tags.push({ tagId: node.tag_id, positionMm: [mx + node.x_mm, my + node.y_mm, mz + node.z_mm], uncertaintyMm: Math.max(0, node.uncertainty_mm) });
  }
  return tags;
}

type Vec3 = readonly [number, number, number];
interface Pair { tagId: number; a: Vec3; b: Vec3; sigmaMm: number }

const distance = (p: Vec3, q: Vec3): number => Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);

function reliableInB(tag: FusedTagObservation | undefined): tag is FusedTagObservation {
  return tag !== undefined && !tag.frameJump && tag.inlierCount >= RELOCK_MIN_OBSERVATIONS && tag.independentViewCount >= RELOCK_MIN_VIEWS &&
    tag.positionScan.every(Number.isFinite);
}

/** Best triangle height over all triples; 0 for fewer than three points. */
function bestTriangleHeightMm(points: readonly Vec3[]): number {
  let best = 0;
  for (let i = 0; i < points.length; i += 1) for (let j = i + 1; j < points.length; j += 1) for (let k = j + 1; k < points.length; k += 1) {
    best = Math.max(best, triangleHeightMm(points[i], points[j], points[k]));
  }
  return best;
}

/** Mean |distance in B - distance in A| per tag, millimetres, and the worst pair against its tolerance. */
function pairAgreement(pairs: readonly Pair[]): { meanDelta: number[]; worstExcessMm: number } {
  const sums = pairs.map(() => 0);
  let worstExcess = -Infinity;
  for (let i = 0; i < pairs.length; i += 1) for (let j = i + 1; j < pairs.length; j += 1) {
    const delta = Math.abs(distance(pairs[i].b, pairs[j].b) - distance(pairs[i].a, pairs[j].a));
    const tolerance = Math.max(RELOCK_MIN_DISTANCE_TOLERANCE_MM, 3 * Math.hypot(pairs[i].sigmaMm, pairs[j].sigmaMm));
    sums[i] += delta; sums[j] += delta;
    worstExcess = Math.max(worstExcess, delta - tolerance);
  }
  const others = Math.max(1, pairs.length - 1);
  return { meanDelta: sums.map((sum) => sum / others), worstExcessMm: worstExcess };
}

/** Closed-form yaw + translation taking B onto A (least squares): the horizontal part is a 2D Procrustes fit, the vertical part a mean offset. */
function fitYawTranslation(pairs: readonly Pair[]): { transformMm: { yaw: number; tx: number; ty: number; tz: number }; rmsMm: number } {
  const n = pairs.length;
  const cb = [0, 0, 0]; const ca = [0, 0, 0];
  for (const pair of pairs) for (let axis = 0; axis < 3; axis += 1) { cb[axis] += pair.b[axis] / n; ca[axis] += pair.a[axis] / n; }
  let sin = 0; let cos = 0;
  for (const pair of pairs) {
    const bx = pair.b[0] - cb[0]; const by = pair.b[1] - cb[1];
    const ax = pair.a[0] - ca[0]; const ay = pair.a[1] - ca[1];
    cos += bx * ax + by * ay;
    sin += bx * ay - by * ax;
  }
  const yaw = Math.atan2(sin, cos);
  const c = Math.cos(yaw); const s = Math.sin(yaw);
  const tx = ca[0] - (c * cb[0] - s * cb[1]);
  const ty = ca[1] - (s * cb[0] + c * cb[1]);
  const tz = ca[2] - cb[2];
  let squares = 0;
  for (const pair of pairs) {
    const px = c * pair.b[0] - s * pair.b[1] + tx; const py = s * pair.b[0] + c * pair.b[1] + ty; const pz = pair.b[2] + tz;
    squares += (px - pair.a[0]) ** 2 + (py - pair.a[1]) ** 2 + (pz - pair.a[2]) ** 2;
  }
  return { transformMm: { yaw, tx, ty, tz }, rmsMm: Math.sqrt(squares / n) };
}

/**
 * The state of the lock for the tags of ONE reference-space epoch of B. `fusion` is the fusion of B's observations (any thresholds: reliability for
 * the lock is judged here); `base` the confirmed tags of A. A caller that sees a reference-space reset starts over with an empty fusion.
 */
export function evaluateRelock(base: readonly RelockBaseTag[], fusion: TagObservationFusion): RelockState {
  const baseById = new Map(base.map((tag) => [tag.tagId, tag]));
  const pairs: Pair[] = [];
  for (const [id, tag] of Object.entries(fusion.byTagId)) {
    const tagId = Number(id);
    const known = baseById.get(tagId);
    if (known === undefined || !reliableInB(tag)) continue;
    const projectM = xrToProject(tag.positionScan);
    pairs.push({
      tagId, a: known.positionMm, b: [projectM[0] * 1000, projectM[1] * 1000, projectM[2] * 1000],
      sigmaMm: Math.hypot(known.uncertaintyMm, tag.spreadM * 1000),
    });
  }
  pairs.sort((x, y) => x.tagId - y.tagId);
  const foundTagIds = pairs.map((pair) => pair.tagId);
  const waiting = (reason: RelockReason, extra: Partial<RelockState> = {}): RelockState => ({
    phase: 'relocalizing', found: pairs.length, needed: RELOCK_MIN_TAGS, foundTagIds, reason, transform: null, rmsMm: null, usedTagIds: [], ...extra,
  });
  if (pairs.length < RELOCK_MIN_TAGS) return waiting('need_more_tags');

  /* A fourth tag is a witness: drop the one that disagrees with the rest while three remain, so one swapped or moved tag cannot block or bend the lock. */
  let used = pairs.slice();
  let lastReason: RelockReason | null = null;
  for (;;) {
    if (bestTriangleHeightMm(used.map((pair) => pair.a)) < RELOCK_MIN_TRIANGLE_HEIGHT_MM ||
        bestTriangleHeightMm(used.map((pair) => pair.b)) < RELOCK_MIN_TRIANGLE_HEIGHT_MM) {
      lastReason = 'collinear';
    } else {
      const agreement = pairAgreement(used);
      if (agreement.worstExcessMm > 0) {
        lastReason = 'distance_mismatch';
        if (used.length > RELOCK_MIN_TAGS) {
          const worst = agreement.meanDelta.indexOf(Math.max(...agreement.meanDelta));
          used = used.filter((_, index) => index !== worst);
          continue;
        }
      } else {
        const fit = fitYawTranslation(used);
        if (fit.rmsMm > RELOCK_MAX_RMS_MM) {
          lastReason = 'residual_too_high';
          if (used.length > RELOCK_MIN_TAGS) {
            /* Leave out the tag with the largest residual and try again. */
            const c = Math.cos(fit.transformMm.yaw); const s = Math.sin(fit.transformMm.yaw);
            const residuals = used.map((pair) => Math.hypot(
              c * pair.b[0] - s * pair.b[1] + fit.transformMm.tx - pair.a[0],
              s * pair.b[0] + c * pair.b[1] + fit.transformMm.ty - pair.a[1],
              pair.b[2] + fit.transformMm.tz - pair.a[2]));
            used = used.filter((_, index) => index !== residuals.indexOf(Math.max(...residuals)));
            continue;
          }
        } else {
          return {
            phase: 'locked', found: pairs.length, needed: RELOCK_MIN_TAGS, foundTagIds, reason: null, rmsMm: fit.rmsMm,
            transform: { yaw: fit.transformMm.yaw, tx: fit.transformMm.tx / 1000, ty: fit.transformMm.ty / 1000, tz: fit.transformMm.tz / 1000 },
            usedTagIds: used.map((pair) => pair.tagId),
          };
        }
      }
    }
    return waiting(lastReason ?? 'need_more_tags');
  }
}

/**
 * Whether a lock that was granted still holds for the tags seen now. A tag that has jumped (the frame shifted), or a re-fit that moves the transform
 * by more than a tolerance, ends the lock: B must not keep feeding A through a frame that has changed under it. A re-fit inside the tolerance is NOT a loss:
 * more sightings of the same tags refine the fit (phone run 2026-11-22: the lock was dropped two seconds after it was found, because tags 0.3 m apart move
 * the yaw by a degree with a 9 mm change of one tag, and the merge then fell back to the ambiguous overlap search).
 */
/** A new fit within this of the held lock is the same lock, better informed: it replaces it. Further away the frame moved and the lock is void. */
export const RELOCK_ADOPT_MAX_SHIFT_MM = 100;
export const RELOCK_ADOPT_MAX_TURN_RAD = 0.05;

export function relockStillHolds(locked: RelockTransform, current: RelockState, toleranceMm = RELOCK_ADOPT_MAX_SHIFT_MM, maxTurnRad = RELOCK_ADOPT_MAX_TURN_RAD): boolean {
  /* Fewer tags in view, or too few reliable ones, contradicts nothing. Three or more tags that disagree with each other or with A do: the frame under B has changed. */
  if (current.phase !== 'locked' || current.transform === null) {
    return !(current.found >= RELOCK_MIN_TAGS && (current.reason === 'distance_mismatch' || current.reason === 'residual_too_high'));
  }
  const shiftMm = Math.hypot(current.transform.tx - locked.tx, current.transform.ty - locked.ty, current.transform.tz - locked.tz) * 1000;
  const turn = Math.abs(Math.atan2(Math.sin(current.transform.yaw - locked.yaw), Math.cos(current.transform.yaw - locked.yaw)));
  return shiftMm <= toleranceMm && turn <= maxTurnRad;
}

/** A tag the lock was fitted to has since jumped: the frame shifted under it, so the lock is void whatever the other tags say. */
export function relockLostByJump(usedTagIds: readonly number[], fusion: TagObservationFusion): boolean {
  return usedTagIds.some((tagId) => fusion.byTagId[tagId as 0 | 1 | 2 | 3 | 4]?.frameJump === true);
}
