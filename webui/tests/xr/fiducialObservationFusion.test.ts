import { describe, expect, it } from 'vitest';
import {
  countIndependentViews, fuseTagObservations, tagObservationViewBucketKey, TAG_OBSERVATION_MIN_BASELINE_METERS, type TagObservation,
} from '../../src/xr/fiducial/tagObservationFusion';

function observedFromView(
  tagId: number,
  positionScan: TagObservation['positionScan'],
  viewIndex: number,
): TagObservation {
  return {
    tagId,
    positionScan,
    cameraPosition: [viewIndex * 0.3, 0, 0],
    cameraForward: [0, 0, 1],
  };
}

describe('fiducial observation fusion', () => {
  it('uses the component median, rejects a distant point by MAD, and reports exact RMS spread', () => {
    const good: TagObservation[] = [
      observedFromView(0, [0, 0, 0], 0),
      observedFromView(0, [0.002, 0, 0], 1),
      observedFromView(0, [0, 0.002, 0], 2),
      observedFromView(0, [0.002, 0.002, 0], 3),
      observedFromView(0, [0.001, 0.001, 0], 4),
      observedFromView(0, [0.2, -0.1, 0.3], 5),
    ];
    const result = fuseTagObservations(good);
    expect(result.rejectedCount).toBe(1);
    expect(result.byTagId[0]).toEqual({
      tagId: 0,
      positionScan: [0.001, 0.001, 0],
      spreadM: Math.sqrt(0.0000016),
      observationCount: 6,
      inlierCount: 5,
      independentViewCount: 5,
      confirmed: true,
    });
  });

  it('confirms at four only when configured for four, and uses five by default', () => {
    const four: TagObservation[] = [0, 1, 2, 3].map((x) => observedFromView(1, [x * 0.001, 0, 0], x));
    expect(fuseTagObservations(four).byTagId[1]?.confirmed).toBe(false);
    expect(fuseTagObservations(four, { minObservations: 4 }).byTagId[1]?.confirmed).toBe(true);
    expect(fuseTagObservations([...four, observedFromView(1, [0.004, 0, 0], 4)]).byTagId[1]?.confirmed).toBe(true);
  });

  it('ignores invalid tag IDs and non-finite positions while counting them as rejected', () => {
    const result = fuseTagObservations([
      { tagId: -1, positionScan: [0, 0, 0] },
      { tagId: 5, positionScan: [0, 0, 0] },
      { tagId: 1.5, positionScan: [0, 0, 0] },
      { tagId: 3, positionScan: [Number.NaN, 0, 0] },
      { tagId: 4, positionScan: [0, 0, 0] },
    ]);
    expect(result.rejectedCount).toBe(4);
    expect(result.byTagId[4]?.positionScan).toEqual([0, 0, 0]);
    expect(result.byTagId[3]).toBeUndefined();
  });

  it('keeps every sample of a symmetric set and reports its real spread (issue #19: no arbitrary single point with zero spread)', () => {
    const ring: TagObservation[] = [[0.02, 0, 0], [-0.02, 0, 0], [0, 0.02, 0], [0, -0.02, 0], [0, 0, 0.02], [0, 0, -0.02]]
      .map((positionScan) => ({ tagId: 2, positionScan }));
    const result = fuseTagObservations(ring);
    expect(result.byTagId[2]?.positionScan).toEqual([0, 0, 0]);
    expect(result.byTagId[2]?.inlierCount).toBe(6);
    expect(result.byTagId[2]?.spreadM).toBeCloseTo(0.02, 12);
    expect(result.rejectedCount).toBe(0);
    expect(result.byTagId[2]?.independentViewCount).toBe(1);
    expect(result.byTagId[2]?.confirmed).toBe(false);
  });

  it('does not confirm repeated detections from one identical camera pose', () => {
    const repeated: TagObservation[] = Array.from({ length: 10 }, () => ({
      tagId: 3,
      positionScan: [1, 0, 0] as const,
      cameraPosition: [0, 0, 0] as const,
      cameraForward: [1, 0, 0] as const,
    }));
    const fused = fuseTagObservations(repeated).byTagId[3]!;
    expect(fused.observationCount).toBe(10);
    expect(fused.inlierCount).toBe(10);
    expect(fused.independentViewCount).toBe(1);
    expect(fused.spreadM).toBe(0);
    expect(fused.confirmed).toBe(false);
  });

  it('confirms agreeing observations from five separated camera views', () => {
    const observations = [0, 1, 2, 3, 4].map((view) =>
      observedFromView(4, [0.2 + view * 0.001, -0.1, 0.3], view));
    const fused = fuseTagObservations(observations).byTagId[4]!;
    expect(fused.independentViewCount).toBe(5);
    expect(fused.confirmed).toBe(true);
    expect(fused.positionScan[0]).toBeCloseTo(0.202, 3);
    expect(fused.positionScan[1]).toBeCloseTo(-0.1, 3);
    expect(fused.positionScan[2]).toBeCloseTo(0.3, 3);
  });

  it('treats provenance-free legacy observations as one shared view', () => {
    const legacy = Array.from({ length: 5 }, () => ({ tagId: 2, positionScan: [0.1, 0.2, 0.3] as const }));
    const fused = fuseTagObservations(legacy).byTagId[2]!;
    expect(fused.inlierCount).toBe(5);
    expect(fused.independentViewCount).toBe(1);
    expect(fused.confirmed).toBe(false);
  });
});

describe('fusion of evenly scattered observations (issue #19)', () => {
  const ring: [number, number, number][] = [[0.02, 0, 0], [-0.02, 0, 0], [0, 0.02, 0], [0, -0.02, 0], [0, 0, 0.02]];
  const fuse = (points: [number, number, number][]) =>
    fuseTagObservations(points.map((point, index) => observedFromView(1, point, index))).byTagId[1]!;

  it('keeps all five plausible 20 mm scattered measurements and reports their real spread, not one point with zero spread', () => {
    const result = fuseTagObservations(ring.map((point, index) => observedFromView(1, point, index)));
    const tag = result.byTagId[1]!;
    expect(result.rejectedCount).toBe(0);
    expect(tag.inlierCount).toBe(5);
    expect(tag.spreadM).toBeGreaterThan(0.01);
    expect(tag.spreadM).toBeLessThan(0.03);
  });

  it('gives the same position and spread for every observation order', () => {
    const reference = fuse(ring);
    const orders: [number, number, number][][] = [
      [...ring].reverse(), [ring[2], ring[4], ring[0], ring[3], ring[1]], [ring[4], ring[3], ring[2], ring[1], ring[0]],
    ];
    for (const order of orders) {
      const other = fuse(order);
      expect(other.spreadM).toBeCloseTo(reference.spreadM, 12);
      other.positionScan.forEach((value, axis) => expect(value).toBeCloseTo(reference.positionScan[axis], 12));
      expect(other.inlierCount).toBe(reference.inlierCount);
    }
  });

  it('still rejects a gross 0.5 m outlier among scattered samples', () => {
    const tag = fuse([...ring, [0.5, 0.5, 0.5]]);
    expect(tag.inlierCount).toBe(5);
    expect(tag.observationCount).toBe(6);
    expect(Math.hypot(...tag.positionScan)).toBeLessThan(0.01);
  });

  it('keeps two identical observations as one tight cluster (median radius 0 falls back to the 5 mm floor)', () => {
    const tag = fuse([[1, 1, 1], [1, 1, 1], [1, 1, 1], [1.002, 1, 1], [2, 2, 2]]);
    expect(tag.inlierCount).toBe(4);
    expect(tag.spreadM).toBeLessThan(0.005);
  });
});

describe('independent views need a real baseline, and the bucket key uses WebXR Y-up axes (issue #17)', () => {
  const at = (cameraForward: [number, number, number], cameraPosition: [number, number, number] = [0, 1.5, 0]): TagObservation =>
    ({ tagId: 1, positionScan: [0, 0, 0], cameraPosition, cameraForward });
  const level: [number, number, number] = [0, 0, -1];
  const up: [number, number, number] = [0, 0.01, -Math.sqrt(1 - 0.0001)];
  const down: [number, number, number] = [0, -0.01, -Math.sqrt(1 - 0.0001)];

  it('does not confirm five sightings from one position with sub-degree tilts (the issue reproduction)', () => {
    const result = fuseTagObservations([level, level, up, up, down].map((forward) => at(forward)));
    const tag = result.byTagId[1]!;
    expect(tag.observationCount).toBe(5);
    expect(tag.independentViewCount).toBe(1);
    expect(tag.confirmed).toBe(false);
  });

  it('does not count a turn on the spot as a new view either', () => {
    const turned: [number, number, number][] = [[0, 0, -1], [1, 0, 0], [0, 0, 1], [-1, 0, 0], [0.7071, 0, -0.7071]];
    const tag = fuseTagObservations(turned.map((forward) => at(forward))).byTagId[1]!;
    expect(tag.independentViewCount).toBe(1);
    expect(tag.confirmed).toBe(false);
  });

  it('counts sightings from positions at least the baseline apart, in any order', () => {
    const observations = [0, 0.3, 0.6, 0.9, 1.2].map((x) => at(level, [x, 1.5, 0]));
    const forward = fuseTagObservations(observations).byTagId[1]!;
    const backward = fuseTagObservations([...observations].reverse()).byTagId[1]!;
    expect(forward.independentViewCount).toBe(5);
    expect(backward.independentViewCount).toBe(5);
    expect(forward.confirmed).toBe(true);
  });

  it('merges camera positions closer than the baseline into one view (boundary jitter between buckets)', () => {
    const jittered = [0.0, 0.04, 0.09, 0.3, 0.34].map((x) => at(level, [x, 1.5, 0]));
    expect(countIndependentViews(jittered)).toBe(2);
    expect(countIndependentViews([at(level, [0, 1.5, 0]), at(level, [TAG_OBSERVATION_MIN_BASELINE_METERS, 1.5, 0])])).toBe(2);
  });

  it('keeps legacy sightings without a pose as a single shared view', () => {
    const legacy: TagObservation[] = [1, 2, 3].map(() => ({ tagId: 1, positionScan: [0, 0, 0] }));
    expect(countIndependentViews(legacy)).toBe(1);
    expect(countIndependentViews([...legacy, at(level, [2, 1.5, 0])])).toBe(2);
  });

  it('buckets the heading about the vertical Y axis and the tilt by the Y component', () => {
    const key = (forward: [number, number, number]) => tagObservationViewBucketKey(at(forward));
    expect(key(level)).toBe(key(up));                    /* a 0.57 degree tilt is the same bucket, not a new heading */
    expect(key(level)).toBe(key(down));
    expect(key([1, 0, 0])).not.toBe(key(level));         /* a 90 degree turn about the vertical is a different heading */
    expect(key([0, 0.5, -Math.sqrt(0.75)])).not.toBe(key(level));   /* a 30 degree look-up is a different elevation */
  });
});
