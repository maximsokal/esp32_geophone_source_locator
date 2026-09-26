import { describe, expect, it } from 'vitest';
import { fuseTagObservations, type TagObservation } from '../../src/xr/fiducial/tagObservationFusion';

describe('fiducial observation fusion', () => {
  it('uses the component median, rejects a distant point by MAD, and reports exact RMS spread', () => {
    const good: TagObservation[] = [
      { tagId: 0, positionScan: [0, 0, 0] },
      { tagId: 0, positionScan: [0.002, 0, 0] },
      { tagId: 0, positionScan: [0, 0.002, 0] },
      { tagId: 0, positionScan: [0.002, 0.002, 0] },
      { tagId: 0, positionScan: [0.001, 0.001, 0] },
      { tagId: 0, positionScan: [0.2, -0.1, 0.3] },
    ];
    const result = fuseTagObservations(good);
    expect(result.rejectedCount).toBe(1);
    expect(result.byTagId[0]).toEqual({
      tagId: 0,
      positionScan: [0.001, 0.001, 0],
      spreadM: Math.sqrt(0.0000016),
      observationCount: 6,
      inlierCount: 5,
      confirmed: true,
    });
  });

  it('confirms at four only when configured for four, and uses five by default', () => {
    const four: TagObservation[] = [0, 1, 2, 3].map((x) => ({ tagId: 1, positionScan: [x * 0.001, 0, 0] }));
    expect(fuseTagObservations(four).byTagId[1]?.confirmed).toBe(false);
    expect(fuseTagObservations(four, { minObservations: 4 }).byTagId[1]?.confirmed).toBe(true);
    expect(fuseTagObservations([...four, { tagId: 1, positionScan: [0.004, 0, 0] }]).byTagId[1]?.confirmed).toBe(true);
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
  });
});
