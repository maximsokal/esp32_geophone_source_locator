import { describe, expect, it } from 'vitest';
import { fuseTagObservations, type TagObservation } from '../../src/xr/fiducial/tagObservationFusion';

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

  it('keeps a finite deterministic center when a symmetric set leaves the MAD gate empty', () => {
    const ring: TagObservation[] = [[0.02, 0, 0], [-0.02, 0, 0], [0, 0.02, 0], [0, -0.02, 0], [0, 0, 0.02], [0, 0, -0.02]]
      .map((positionScan) => ({ tagId: 2, positionScan }));
    const result = fuseTagObservations(ring);
    expect(result.byTagId[2]?.positionScan).toEqual([0.02, 0, 0]);
    expect(result.byTagId[2]?.inlierCount).toBe(1);
    expect(result.byTagId[2]?.spreadM).toBe(0);
    expect(result.rejectedCount).toBe(5);
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
