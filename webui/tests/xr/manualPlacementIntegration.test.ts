import { afterEach, describe, expect, it } from 'vitest';

import {
  ManualControlPointStore,
  ManualPlacementRejectedError,
} from '../../src/xr/manualControlPoints';
import {
  registerManualPlacementResolver,
  type ManualPlacementResolution,
} from '../../src/xr/manualPlacementSurface';

afterEach(() => registerManualPlacementResolver(null));

function resolution(
  accepted: boolean,
  position: readonly [number, number, number] | null,
): ManualPlacementResolution {
  return {
    accepted,
    position,
    mode: accepted ? 'edge' : null,
    confidence: accepted ? 0.91 : 0,
    supportCount: accepted ? 12 : 1,
    sourceDistanceMeters: accepted ? 0.02 : 0.4,
    message: accepted ? 'locked' : 'scan more first',
  };
}

describe('manual control point surface lock', () => {
  it('rejects a raw tap instead of creating a pin when stable geometry is missing', () => {
    registerManualPlacementResolver(() => resolution(false, null));
    const store = new ManualControlPointStore();
    expect(() => store.addOrSnap([1, 2, 3])).toThrow(ManualPlacementRejectedError);
    expect(store.size).toBe(0);
  });

  it('stores the geometry-locked solution rather than the raw depth point', () => {
    registerManualPlacementResolver(() => resolution(true, [0.5, 0.75, 1.25]));
    const store = new ManualControlPointStore();
    const result = store.addOrSnap([9, 9, 9]);
    expect(result.created).toBe(true);
    expect(result.snapped).toBe(true);
    expect(result.placementMode).toBe('edge');
    expect(result.point.position).toEqual([0.5, 0.75, 1.25]);
  });

  it('applies the same stable-surface gate while moving an existing pin', () => {
    registerManualPlacementResolver(null);
    const store = new ManualControlPointStore();
    const point = store.add([0, 0, 0]);

    registerManualPlacementResolver(() => resolution(false, null));
    expect(() => store.update(point.id, [3, 3, 3])).toThrow(ManualPlacementRejectedError);
    expect(store.snapshot()[0].position).toEqual([0, 0, 0]);
  });
});
