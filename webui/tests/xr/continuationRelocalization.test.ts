import { describe, expect, it } from 'vitest';

import {
  evaluateRelock,
  INITIAL_RELOCK_STATE,
  RELOCK_MAX_RMS_MM,
  relockBaseTags,
  relockLostByJump,
  relockStillHolds,
  type RelockBaseTag,
  type RelockTransform,
} from '../../src/xr/fiducial/continuationRelocalization';
import type { FusedTagObservation, TagObservationFusion } from '../../src/xr/fiducial/tagObservationFusion';
import type { TagPositionsArtifact } from '../../src/xr/fiducial/tagPositionsArtifact';

/* Ground truth: tags in the saved scan A (project frame, Z up, mm) and the same tags as session B sees them in WebXR local-floor (Y up, m). */
const A: readonly RelockBaseTag[] = [
  { tagId: 0, positionMm: [0, 0, 0], uncertaintyMm: 2 },
  { tagId: 1, positionMm: [1500, 200, 30], uncertaintyMm: 3 },
  { tagId: 2, positionMm: [400, 1800, -20], uncertaintyMm: 3 },
  { tagId: 3, positionMm: [-900, 700, 50], uncertaintyMm: 4 },
  { tagId: 4, positionMm: [800, -1200, 10], uncertaintyMm: 4 },
];

/** B -> A is `truth`; the inverse maps an A position into B, then project -> WebXR (x, z, -y). */
function toB(positionMm: readonly [number, number, number], truth: RelockTransform): [number, number, number] {
  const x = positionMm[0] / 1000 - truth.tx; const y = positionMm[1] / 1000 - truth.ty; const z = positionMm[2] / 1000 - truth.tz;
  const c = Math.cos(-truth.yaw); const s = Math.sin(-truth.yaw);
  const px = c * x - s * y; const py = s * x + c * y;
  return [px, z, -py];                                   /* project (px, py, z) -> WebXR local-floor */
}

function fused(tagId: number, positionScan: readonly [number, number, number], overrides: Partial<FusedTagObservation> = {}): FusedTagObservation {
  return {
    tagId, positionScan: [...positionScan], spreadM: 0.002, observationCount: 8, inlierCount: 8, independentViewCount: 4,
    confirmed: true, frameJump: false, jumpSeparationM: null, ...overrides,
  };
}
const fusionOf = (...tags: FusedTagObservation[]): TagObservationFusion => ({
  byTagId: Object.fromEntries(tags.map((tag) => [tag.tagId, tag])) as TagObservationFusion['byTagId'], rejectedCount: 0,
});
const TRUTH: RelockTransform = { yaw: 0.7, tx: 2.5, ty: -1.2, tz: 0.04 };
const seen = (ids: number[], truth = TRUTH, tweak: (id: number, p: [number, number, number]) => [number, number, number] = (_, p) => p) =>
  fusionOf(...ids.map((id) => fused(id, tweak(id, toB(A.find((tag) => tag.tagId === id)!.positionMm, truth)))));

describe('tag-locked re-localisation of a Continue scan (issue #45, gate 1)', () => {
  it('starts waiting: nothing seen is 0/3', () => {
    const state = evaluateRelock(A, fusionOf());
    expect(state).toMatchObject({ phase: 'relocalizing', found: 0, needed: 3, reason: 'need_more_tags', transform: null });
    expect(INITIAL_RELOCK_STATE).toMatchObject({ phase: 'relocalizing', found: 0, needed: 3 });
  });

  it('counts DISTINCT known tags: one tag seen eight times is one, two tags are not enough', () => {
    const one = evaluateRelock(A, fusionOf(fused(0, toB(A[0].positionMm, TRUTH), { observationCount: 80, inlierCount: 80, independentViewCount: 20 })));
    expect(one).toMatchObject({ phase: 'relocalizing', found: 1, reason: 'need_more_tags' });
    expect(evaluateRelock(A, seen([0, 1]))).toMatchObject({ phase: 'relocalizing', found: 2, foundTagIds: [0, 1], reason: 'need_more_tags' });
  });

  it('does not count tags A does not know, A did not confirm, or B has not seen reliably', () => {
    const unknown = fusionOf(fused(7, [1, 1, -1]), fused(8, [2, 1, -1]), fused(9, [1, 1, -2]));
    expect(evaluateRelock(A, unknown).found).toBe(0);
    const onlyTwoKnown = [A[0], A[1]];
    expect(evaluateRelock(onlyTwoKnown, seen([0, 1, 2])).found).toBe(2);       /* tag 2 is not a confirmed tag of A */
    const provisional = fusionOf(
      fused(0, toB(A[0].positionMm, TRUTH)),
      fused(1, toB(A[1].positionMm, TRUTH), { inlierCount: 2, observationCount: 2, independentViewCount: 1, confirmed: false }),
      fused(2, toB(A[2].positionMm, TRUTH), { independentViewCount: 1 }),
      fused(3, toB(A[3].positionMm, TRUTH), { frameJump: true, jumpSeparationM: 4.2 }),
    );
    expect(evaluateRelock(A, provisional)).toMatchObject({ phase: 'relocalizing', found: 1, foundTagIds: [0] });
    const nonFinite = fusionOf(fused(0, [Number.NaN, 0, 0]), fused(1, toB(A[1].positionMm, TRUTH)), fused(2, toB(A[2].positionMm, TRUTH)));
    expect(evaluateRelock(A, nonFinite).found).toBe(2);
  });

  it('needs several sightings AND several views: either alone is not a reliable tag', () => {
    const fewSightings = fusionOf(
      fused(0, toB(A[0].positionMm, TRUTH)), fused(1, toB(A[1].positionMm, TRUTH)),
      fused(2, toB(A[2].positionMm, TRUTH), { inlierCount: 2, observationCount: 2, independentViewCount: 6 }),   /* many views, too few sightings */
    );
    expect(evaluateRelock(A, fewSightings)).toMatchObject({ phase: 'relocalizing', found: 2, foundTagIds: [0, 1] });
    const oneView = fusionOf(
      fused(0, toB(A[0].positionMm, TRUTH)), fused(1, toB(A[1].positionMm, TRUTH)),
      fused(2, toB(A[2].positionMm, TRUTH), { inlierCount: 30, observationCount: 30, independentViewCount: 1 }),   /* many sightings, one view */
    );
    expect(evaluateRelock(A, oneView)).toMatchObject({ phase: 'relocalizing', found: 2 });
  });

  it.each([
    { yaw: 0, tx: 0, ty: 0, tz: 0 },
    { yaw: 0.7, tx: 2.5, ty: -1.2, tz: 0.04 },
    { yaw: -2.9, tx: -8, ty: 5, tz: -0.3 },
    { yaw: Math.PI - 1e-3, tx: 1, ty: 1, tz: 0 },         /* near the +-pi wrap */
    { yaw: -Math.PI + 1e-3, tx: 0.2, ty: -0.4, tz: 0.1 },
  ])('locks on three trusted, non-collinear tags and recovers the exact transform (%j)', (truth) => {
    const state = evaluateRelock(A, seen([0, 2, 3], truth));
    expect(state.phase).toBe('locked');
    expect(state.reason).toBeNull();
    expect(state.rmsMm!).toBeLessThan(1e-6);
    const t = state.transform!;
    expect(Math.atan2(Math.sin(t.yaw - truth.yaw), Math.cos(t.yaw - truth.yaw))).toBeCloseTo(0, 9);
    expect(t.tx).toBeCloseTo(truth.tx, 9);
    expect(t.ty).toBeCloseTo(truth.ty, 9);
    expect(t.tz).toBeCloseTo(truth.tz, 9);
    expect(state.usedTagIds).toEqual([0, 2, 3]);
  });

  it('gives the same transform in any order the tags arrive in', () => {
    const forward = evaluateRelock(A, seen([0, 1, 2, 3]));
    const shuffled = evaluateRelock(A, seen([3, 1, 0, 2]));
    expect(shuffled).toEqual(forward);
  });

  it('still locks with realistic noise, with an error well below the gate', () => {
    let seed = 7;
    const noise = (): number => { seed = (seed * 1664525 + 1013904223) >>> 0; return ((seed / 0xffffffff) - 0.5) * 0.006; };   /* +-3 mm */
    const state = evaluateRelock(A, seen([0, 1, 2, 3, 4], TRUTH, (_, p) => [p[0] + noise(), p[1] + noise(), p[2] + noise()]));
    expect(state.phase).toBe('locked');
    expect(state.rmsMm!).toBeLessThan(RELOCK_MAX_RMS_MM / 2);
    expect(Math.hypot(state.transform!.tx - TRUTH.tx, state.transform!.ty - TRUTH.ty, state.transform!.tz - TRUTH.tz) * 1000).toBeLessThan(6);
    expect(Math.abs(state.transform!.yaw - TRUTH.yaw)).toBeLessThan(0.005);
  });

  it('refuses tags on a line: three correspondences do not fix a rotation', () => {
    const line: RelockBaseTag[] = [0, 1, 2].map((i) => ({ tagId: i, positionMm: [i * 1000, 0, 0] as const, uncertaintyMm: 2 }));
    const fusion = fusionOf(...line.map((tag) => fused(tag.tagId, toB(tag.positionMm, TRUTH))));
    expect(evaluateRelock(line, fusion)).toMatchObject({ phase: 'relocalizing', reason: 'collinear', found: 3, transform: null });
  });

  it('applies the triangle-height gate exactly at its bound (30 mm)', () => {
    const triangle = (offset: number): RelockBaseTag[] => [
      { tagId: 0, positionMm: [0, 0, 0], uncertaintyMm: 2 }, { tagId: 1, positionMm: [2000, 0, 0], uncertaintyMm: 2 }, { tagId: 2, positionMm: [1000, offset, 0], uncertaintyMm: 2 },
    ];
    const run = (offset: number) => {
      const base = triangle(offset);
      return evaluateRelock(base, fusionOf(...base.map((tag) => fused(tag.tagId, toB(tag.positionMm, TRUTH)))));
    };
    expect(run(29).reason).toBe('collinear');
    expect(run(31).phase).toBe('locked');
  });

  it('refuses a B whose tag distances differ from A (wrong scale, or tags that were moved)', () => {
    const scaled = seen([0, 1, 2], TRUTH, (_, p) => [p[0] * 1.05, p[1] * 1.05, p[2] * 1.05]);
    expect(evaluateRelock(A, scaled)).toMatchObject({ phase: 'relocalizing', reason: 'distance_mismatch', transform: null });
    const moved = seen([0, 1, 2], TRUTH, (id, p) => (id === 1 ? [p[0] + 0.2, p[1], p[2]] : p));
    expect(evaluateRelock(A, moved)).toMatchObject({ phase: 'relocalizing', reason: 'distance_mismatch' });
  });

  it('refuses two tags swapped between A and B: the distances to the third tag give it away', () => {
    const swapped = fusionOf(
      fused(0, toB(A[0].positionMm, TRUTH)),
      fused(1, toB(A[2].positionMm, TRUTH)),                /* tag 1 is where A has tag 2 ... */
      fused(2, toB(A[1].positionMm, TRUTH)),                /* ... and the other way round */
    );
    expect(evaluateRelock(A, swapped).phase).toBe('relocalizing');
  });

  it('refuses a mirrored B (same distances, opposite handedness) and a tilted B: yaw and translation only', () => {
    const mirrored = seen([0, 1, 2], TRUTH, (_, p) => [-p[0], p[1], p[2]]);
    expect(evaluateRelock(A, mirrored)).toMatchObject({ phase: 'relocalizing', reason: 'residual_too_high' });
    const tilt = (degrees: number) => (_: number, p: [number, number, number]): [number, number, number] => {
      const c = Math.cos((degrees * Math.PI) / 180); const s = Math.sin((degrees * Math.PI) / 180);
      return [p[0], c * p[1] - s * p[2], s * p[1] + c * p[2]];
    };
    expect(evaluateRelock(A, seen([0, 1, 2], TRUTH, tilt(5))).phase).toBe('relocalizing');
  });

  it('uses a fourth tag as a witness: one wrong tag is dropped, not allowed to bend or block the lock', () => {
    const withWrongFourth = seen([0, 1, 2, 3], TRUTH, (id, p) => (id === 3 ? [p[0] + 0.3, p[1], p[2] - 0.2] : p));
    const state = evaluateRelock(A, withWrongFourth);
    expect(state.phase).toBe('locked');
    expect(state.usedTagIds).toEqual([0, 1, 2]);
    expect(state.found).toBe(4);
    expect(state.transform!.tx).toBeCloseTo(TRUTH.tx, 9);
    expect(state.transform!.yaw).toBeCloseTo(TRUTH.yaw, 9);
  });

  it('a tag that was seen to jump never counts toward a lock', () => {
    const jumped = fusionOf(
      fused(0, toB(A[0].positionMm, TRUTH)), fused(1, toB(A[1].positionMm, TRUTH)),
      fused(2, toB(A[2].positionMm, TRUTH), { frameJump: true, jumpSeparationM: 4.2 }),
    );
    expect(evaluateRelock(A, jumped)).toMatchObject({ phase: 'relocalizing', found: 2 });
    expect(relockLostByJump([0, 1, 2], jumped)).toBe(true);
    expect(relockLostByJump([0, 1], jumped)).toBe(false);
  });
});

describe('saved tags as the anchors of a re-lock', () => {
  const artifact = (overrides: Partial<TagPositionsArtifact> = {}): TagPositionsArtifact => ({
    schema: 1, kind: 'tag-positions', frame: 'sensor_z_up', scan_frame_id: 'scan-crc32:aaaaaaaa', created_at_ms: 1, geometry_computed_at_ms: null,
    main_scan_position_mm: [1000, 2000, 300], weakest_main_triangle_height_mm: 400,
    nodes: [
      { node_id: 0, tag_id: 0, x_mm: 0, y_mm: 0, z_mm: 0, uncertainty_mm: 0, confirmed: true, observations: 9, views: 4 },
      { node_id: 1, tag_id: 1, x_mm: 1500, y_mm: -200, z_mm: 20, uncertainty_mm: 3, confirmed: true, observations: 9, views: 4 },
      { node_id: 2, tag_id: 2, x_mm: 300, y_mm: 900, z_mm: -10, uncertainty_mm: 5, confirmed: false, observations: 3, views: 1 },
    ],
    ...overrides,
  } as TagPositionsArtifact);

  it('takes only the confirmed nodes, in the scan frame: MAIN position plus the layout offset', () => {
    const tags = relockBaseTags(artifact(), 'scan-crc32:aaaaaaaa');
    expect(tags.map((tag) => tag.tagId)).toEqual([0, 1]);
    expect(tags[1].positionMm).toEqual([2500, 1800, 320]);
    expect(tags[1].uncertaintyMm).toBe(3);
  });

  it('gives no anchors for a layout saved for another scan, with no scan id, or without a usable MAIN position', () => {
    expect(relockBaseTags(artifact(), 'scan-crc32:bbbbbbbb')).toEqual([]);
    expect(relockBaseTags(artifact({ scan_frame_id: null }), null)).toEqual([]);
    expect(relockBaseTags(null, 'scan-crc32:aaaaaaaa')).toEqual([]);
    expect(relockBaseTags(artifact({ main_scan_position_mm: [Number.NaN, 0, 0] }), 'scan-crc32:aaaaaaaa')).toEqual([]);
  });
});

describe('does a granted lock still hold', () => {
  const locked: RelockTransform = { yaw: 0.7, tx: 2.5, ty: -1.2, tz: 0.04 };
  const state = (transform: RelockTransform) => evaluateRelock(A, seen([0, 1, 2], transform));

  it('holds for the same transform, and when fewer tags are in view (nothing contradicts it)', () => {
    expect(relockStillHolds(locked, state(locked))).toBe(true);
    expect(relockStillHolds(locked, evaluateRelock(A, seen([0])))).toBe(true);
  });

  it('ends for a transform that moved: a 100 mm shift or a 3 degree turn', () => {
    expect(relockStillHolds(locked, state({ ...locked, tx: locked.tx + 0.1 }))).toBe(false);
    expect(relockStillHolds(locked, state({ ...locked, yaw: locked.yaw + 0.05 }))).toBe(false);
    expect(relockStillHolds(locked, state({ ...locked, tx: locked.tx + 0.01 }))).toBe(true);     /* 10 mm: noise */
  });
});
