import { describe, expect, it } from 'vitest';

import { evaluateRelock, type RelockBaseTag, type RelockTransform } from '../../src/xr/fiducial/continuationRelocalization';
import {
  baseToExtensionLocalFloor,
  buildReferenceOverlay,
  REFERENCE_OVERLAY_CONFIDENCE,
  REFERENCE_OVERLAY_MAX_POINTS,
} from '../../src/xr/fiducial/referenceOverlay';
import type { TagObservationFusion } from '../../src/xr/fiducial/tagObservationFusion';
import { xrToProject } from '../../src/xr/fiducial/liveTagTracker';

const TRUTH: RelockTransform = { yaw: 0.7, tx: 2.5, ty: -1.2, tz: 0.04 };

/** B -> A (the lock's direction) applied to a point of B in project metres. */
function toBase(p: readonly [number, number, number], t: RelockTransform): [number, number, number] {
  const c = Math.cos(t.yaw); const s = Math.sin(t.yaw);
  return [c * p[0] - s * p[1] + t.tx, s * p[0] + c * p[1] + t.ty, p[2] + t.tz];
}

describe('the saved scan A drawn over the live camera of B (issue #45, gate 2)', () => {
  it('is the exact inverse of the lock: a point of B taken into A and back lands where it was seen', () => {
    for (const truth of [TRUTH, { yaw: 0, tx: 0, ty: 0, tz: 0 }, { yaw: -2.9, tx: -8, ty: 5, tz: -0.3 }, { yaw: Math.PI - 1e-3, tx: 1, ty: 1, tz: 0 }]) {
      for (const seenInB of [[0, 0, 0], [1, 2, 0.5], [-3, 4, -0.2]] as const) {
        const inA = toBase(seenInB, truth);
        const local = baseToExtensionLocalFloor(inA, truth);
        const expected = [seenInB[0], seenInB[2], -seenInB[1]];            /* project (x, y, z) -> WebXR (x, z, -y) */
        for (let axis = 0; axis < 3; axis += 1) expect(local[axis]).toBeCloseTo(expected[axis], 9);
      }
    }
  });

  it('agrees with the lock that the same tags produced: a tag of A is drawn where session B sees that tag', () => {
    const A: RelockBaseTag[] = [
      { tagId: 0, positionMm: [0, 0, 0], uncertaintyMm: 2 },
      { tagId: 1, positionMm: [1500, 200, 30], uncertaintyMm: 3 },
      { tagId: 2, positionMm: [400, 1800, -20], uncertaintyMm: 3 },
    ];
    const seen = (tag: RelockBaseTag): [number, number, number] => {
      /* the tag as B sees it in WebXR local-floor: A -> B project -> local-floor */
      const x = tag.positionMm[0] / 1000 - TRUTH.tx; const y = tag.positionMm[1] / 1000 - TRUTH.ty; const z = tag.positionMm[2] / 1000 - TRUTH.tz;
      const c = Math.cos(-TRUTH.yaw); const s = Math.sin(-TRUTH.yaw);
      return [c * x - s * y, z, -(s * x + c * y)];
    };
    const fusion: TagObservationFusion = {
      byTagId: Object.fromEntries(A.map((tag) => [tag.tagId, {
        tagId: tag.tagId, positionScan: seen(tag), spreadM: 0.002, observationCount: 8, inlierCount: 8, independentViewCount: 4, confirmed: true, frameJump: false, jumpSeparationM: null,
      }])) as TagObservationFusion['byTagId'],
      rejectedCount: 0,
    };
    const lock = evaluateRelock(A, fusion);
    expect(lock.phase).toBe('locked');
    const overlay = buildReferenceOverlay(Float32Array.from(A.flatMap((tag) => tag.positionMm.map((mm) => mm / 1000))), lock.transform)!;
    expect(overlay.pointCount).toBe(3);
    A.forEach((tag, index) => {
      const expected = seen(tag);
      for (let axis = 0; axis < 3; axis += 1) expect(overlay.positions[index * 3 + axis]).toBeCloseTo(expected[axis], 4);
    });
    /* and WebXR -> project of what is drawn gives back B's project coordinates of that tag */
    const drawn = xrToProject([overlay.positions[3], overlay.positions[4], overlay.positions[5]]);
    const inB = toBase(drawn, TRUTH);                       /* B -> A of the drawn point is A's own position of the tag */
    expect(inB[0] * 1000).toBeCloseTo(A[1].positionMm[0], 2);
    expect(inB[1] * 1000).toBeCloseTo(A[1].positionMm[1], 2);
    expect(inB[2] * 1000).toBeCloseTo(A[1].positionMm[2], 2);
  });

  it('draws nothing for an unusable transform or no points, and skips damaged samples', () => {
    const points = Float32Array.from([1, 1, 1, 2, 2, 2]);
    expect(buildReferenceOverlay(points, null)).toBeNull();
    expect(buildReferenceOverlay(points, { ...TRUTH, yaw: Number.NaN })).toBeNull();
    expect(buildReferenceOverlay(new Float32Array(0), TRUTH)).toBeNull();
    expect(buildReferenceOverlay(Float32Array.from([Number.NaN, 0, 0, Infinity, 0, 0]), TRUTH)).toBeNull();
    const mixed = buildReferenceOverlay(Float32Array.from([Number.NaN, 0, 0, 1, 2, 3]), TRUTH)!;
    expect(mixed.pointCount).toBe(1);
    expect(Array.from(mixed.positions).every(Number.isFinite)).toBe(true);
  });

  it('is a bounded, even decimation with one constant confidence, so it never costs more than the cap', () => {
    const many = new Float32Array(50_000 * 3);
    for (let i = 0; i < 50_000; i += 1) { many[i * 3] = i * 0.001; many[i * 3 + 1] = 0; many[i * 3 + 2] = 1; }
    const overlay = buildReferenceOverlay(many, TRUTH)!;
    expect(overlay.pointCount).toBeLessThanOrEqual(REFERENCE_OVERLAY_MAX_POINTS);
    expect(overlay.pointCount).toBeGreaterThan(REFERENCE_OVERLAY_MAX_POINTS / 2);
    expect(overlay.positions.length).toBe(overlay.pointCount * 3);
    expect(overlay.confidence.length).toBe(overlay.pointCount);
    expect(new Set(overlay.confidence)).toEqual(new Set([REFERENCE_OVERLAY_CONFIDENCE]));
    const first = baseToExtensionLocalFloor([many[0], many[1], many[2]], TRUTH);
    expect(overlay.positions[0]).toBeCloseTo(first[0], 5);                     /* the first sample is kept */
    expect(buildReferenceOverlay(many, TRUTH, 100)!.pointCount).toBeLessThanOrEqual(100);
  });

  it('does not alter its input', () => {
    const points = Float32Array.from([1, 2, 3, 4, 5, 6]);
    const before = Array.from(points);
    buildReferenceOverlay(points, TRUTH);
    expect(Array.from(points)).toEqual(before);
  });
});
