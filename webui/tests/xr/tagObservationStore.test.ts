import { describe, expect, it } from 'vitest';

import { fuseTagObservations, type TagObservation } from '../../src/xr/fiducial/tagObservationFusion';
import {
  TAG_OBSERVATION_STORE_DEFAULT_PER_TAG_BUDGET,
  TAG_OBSERVATION_STORE_DEFAULT_TOTAL_BUDGET,
  TagObservationStore,
} from '../../src/xr/fiducial/tagObservationStore';

function viewedObservation(
  tagId: number,
  positionScan: TagObservation['positionScan'],
  cameraX: number,
  options: Pick<TagObservation, 'capturedAtMs' | 'quality'> = {},
): TagObservation {
  return {
    tagId,
    positionScan,
    cameraPosition: [cameraX, tagId * 2, 1],
    cameraForward: [0, 0, -1],
    ...options,
  };
}

function countMedianSortComparisons(observations: readonly TagObservation[]): number {
  const originalSort = Array.prototype.sort;
  let comparisons = 0;
  const instrumentedSort = function <T>(this: T[], compareFn?: (left: T, right: T) => number): T[] {
    if (compareFn === undefined) return Reflect.apply(originalSort, this, []) as T[];
    const wrapped = (left: T, right: T): number => {
      comparisons += 1;
      return compareFn(left, right);
    };
    return Reflect.apply(originalSort, this, [wrapped]) as T[];
  };
  Array.prototype.sort = instrumentedSort as typeof Array.prototype.sort;
  try {
    fuseTagObservations(observations);
  } finally {
    Array.prototype.sort = originalSort;
  }
  return comparisons;
}

describe('tag observation store', () => {
  it('uses the documented hard per-tag and total budgets', () => {
    const defaults = new TagObservationStore();
    expect(defaults.perTagBudget).toBe(48);
    expect(defaults.totalBudget).toBe(256);
    expect(TAG_OBSERVATION_STORE_DEFAULT_PER_TAG_BUDGET).toBe(48);
    expect(TAG_OBSERVATION_STORE_DEFAULT_TOTAL_BUDGET).toBe(256);

    const store = new TagObservationStore({ perTagBudget: 4, totalBudget: 2 });
    store.insert(viewedObservation(0, [0, 0, 0], 0));
    store.insert(viewedObservation(0, [0, 0, 0], 0.15));
    store.insert(viewedObservation(1, [1, 0, 0], 10));
    expect(store.retainedCount).toBe(2);
    expect(store.retainedCountForTag(0)).toBeLessThanOrEqual(4);
    expect(store.retainedCountForTag(1)).toBeLessThanOrEqual(4);
    expect(store.getObservations().map((item) => item.tagId)).toEqual([0, 1]);
  });

  it('keeps one best representative per pose bucket and resolves quality ties by age', () => {
    const store = new TagObservationStore();
    const oldest = viewedObservation(0, [0.1, 0, 0], 0, { capturedAtMs: 10, quality: 0.7 });
    const best = viewedObservation(0, [0.2, 0, 0], 0, { capturedAtMs: 20, quality: 0.9 });
    const laterTie = viewedObservation(0, [0.3, 0, 0], 0, { capturedAtMs: 30, quality: 0.9 });

    store.insert(oldest);
    store.insert(best);
    store.insert(laterTie);
    expect(store.retainedCountForTag(0)).toBe(1);
    expect(store.getObservations(0)).toEqual([best]);
    expect(store.rejectedCount).toBe(2);
  });

  it('keeps sightings of different directions as separate buckets, but they are one independent view without a camera baseline (issue #17)', () => {
    const store = new TagObservationStore();
    store.insert({ tagId: 0, positionScan: [0, 0, 0], cameraPosition: [0, 0, 0], cameraForward: [1, 0, 0] });
    store.insert({ tagId: 0, positionScan: [0, 0, 0], cameraPosition: [0, 0, 0], cameraForward: [-1, 0, 0] });

    expect(store.getObservations(0)).toHaveLength(2);
    expect(fuseTagObservations(store.getObservations(0)).byTagId[0]?.independentViewCount).toBe(1);   /* turning on the spot adds no baseline */
  });

  it('evicts the most redundant neighboring view before a distinct distant view', () => {
    const store = new TagObservationStore({ perTagBudget: 2, totalBudget: 2 });
    store.insert(viewedObservation(0, [0, 0, 0], 0));
    store.insert(viewedObservation(0, [0, 0, 0], 0.15));
    store.insert(viewedObservation(0, [0, 0, 0], 10));

    expect(store.getObservations(0).map((item) => item.cameraPosition?.[0])).toEqual([0, 10]);
    expect(store.evictedCount).toBe(1);
  });

  it('does not let 100 same-pose repeats dominate four independent views', () => {
    const store = new TagObservationStore();
    for (let repeat = 0; repeat < 100; repeat += 1) {
      store.insert(viewedObservation(1, [0.2, 0, 0], 0, { capturedAtMs: repeat }));
    }
    for (let view = 0; view < 4; view += 1) {
      store.insert(viewedObservation(1, [view * 0.001, 0, 0], 0.3 + view * 0.3));
    }

    const retained = store.getObservations(1);
    const fused = fuseTagObservations(retained).byTagId[1]!;
    expect(retained).toHaveLength(5);
    expect(store.rejectedCount).toBe(99);
    expect(fused.inlierCount).toBe(4);
    expect(fused.independentViewCount).toBe(4);
    expect(fused.positionScan[0]).toBeCloseTo(0.0015, 4);
    expect(fused.confirmed).toBe(false);
  });

  it('keeps a 30-minute, 3-Hz, five-tag session bounded, deterministic, and flat-cost after filling', () => {
    const store = new TagObservationStore();
    const replay = new TagObservationStore();
    const firstCycle: TagObservation[] = [];
    let remainedWithinBudgets = true;
    let comparisonsAfterFill = 0;

    for (let frame = 0; frame < 30 * 60 * 3; frame += 1) {
      const view = frame % 48;
      for (let tagId = 0; tagId < 5; tagId += 1) {
        const observation = viewedObservation(
          tagId,
          [tagId * 0.25 + Math.sin(view) * 0.002, 0.5, -0.2],
          view * 0.2,
          { capturedAtMs: frame * 1000 / 3, quality: 0.9 },
        );
        store.insert(observation);
        replay.insert(observation);
        if (frame < 48) firstCycle.push(observation);
      }
      remainedWithinBudgets = remainedWithinBudgets && store.retainedCount <= store.totalBudget &&
        [0, 1, 2, 3, 4].every((tagId) => store.retainedCountForTag(tagId) <= store.perTagBudget);
      if (frame === 47) comparisonsAfterFill = countMedianSortComparisons(store.getObservations());
    }

    const retained = store.getObservations();
    expect(remainedWithinBudgets).toBe(true);
    expect(retained).toHaveLength(5 * 48);
    expect(retained.length).toBeLessThanOrEqual(store.totalBudget);
    expect(JSON.stringify(retained)).toBe(JSON.stringify(replay.getObservations()));
    expect(countMedianSortComparisons(retained)).toBe(comparisonsAfterFill);

    const offline = fuseTagObservations(firstCycle).byTagId;
    const bounded = fuseTagObservations(retained).byTagId;
    for (let tagId = 0; tagId < 5; tagId += 1) {
      expect(bounded[tagId]?.positionScan[0]).toBeCloseTo(offline[tagId]!.positionScan[0], 6);
      expect(bounded[tagId]?.positionScan[1]).toBeCloseTo(offline[tagId]!.positionScan[1], 6);
      expect(bounded[tagId]?.positionScan[2]).toBeCloseTo(offline[tagId]!.positionScan[2], 6);
      expect(bounded[tagId]?.independentViewCount).toBe(48);
      expect(bounded[tagId]?.confirmed).toBe(true);
    }
  });

  it('accumulates rejected and outlier statistics without retaining rejected samples', () => {
    const store = new TagObservationStore();
    store.insert(viewedObservation(0, [0, 0, 0], 0));
    store.recordRejected(2);
    store.recordOutliers(3);
    expect(store.retainedCount).toBe(1);
    expect(store.rejectedCount).toBe(5);
    expect(store.outlierCount).toBe(3);
  });
});
