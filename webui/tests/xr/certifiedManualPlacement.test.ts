import { afterEach, describe, expect, it } from 'vitest';

import {
  authorizeCertifiedManualPlacement,
  ManualControlPointStore,
  ManualPlacementRejectedError,
} from '../../src/xr/manualControlPoints';
import { registerManualPlacementResolver } from '../../src/xr/manualPlacementSurface';

afterEach(() => registerManualPlacementResolver(null));

function rejectEverything() {
  registerManualPlacementResolver(() => ({
    accepted: false,
    position: null,
    mode: null,
    confidence: 0,
    supportCount: 0,
    sourceDistanceMeters: null,
    message: 'three-plane resolver rejects this two-plane endpoint',
  }));
}

describe('one-shot certified structural placement', () => {
  it('stores the exact externally certified endpoint without invoking the three-plane resolver', () => {
    rejectEverything();
    const store = new ManualControlPointStore();
    authorizeCertifiedManualPlacement([0.4, 0.2, 0.8], 0.86);
    const result = store.addOrSnap([0.4, 0.2, 0.8]);
    expect(result.created).toBe(true);
    expect(result.snapped).toBe(true);
    expect(result.placementConfidence).toBeCloseTo(0.86, 6);
    expect(result.point.position).toEqual([0.4, 0.2, 0.8]);
  });

  it('does not let a different coordinate consume the certified endpoint token', () => {
    rejectEverything();
    const store = new ManualControlPointStore();
    authorizeCertifiedManualPlacement([0.4, 0.2, 0.8], 0.86);
    expect(() => store.addOrSnap([0.5, 0.2, 0.8])).toThrow(ManualPlacementRejectedError);
    expect(store.size).toBe(0);
  });

  it('consumes the authorization after one placement', () => {
    rejectEverything();
    const store = new ManualControlPointStore();
    authorizeCertifiedManualPlacement([0.4, 0.2, 0.8], 0.86);
    store.addOrSnap([0.4, 0.2, 0.8]);
    expect(() => store.addOrSnap([0.4, 0.2, 0.8])).toThrow(ManualPlacementRejectedError);
    expect(store.size).toBe(1);
  });
});
