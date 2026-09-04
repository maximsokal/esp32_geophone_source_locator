import { afterEach, describe, expect, it } from 'vitest';

import {
  deactivateCertifiedGuideRegistry,
  setCertifiedGuideCorners,
  setCertifiedGuideEdges,
} from '../../src/xr/certifiedCornerRegistry';
import {
  authorizeCertifiedManualPlacement,
  ManualControlPointStore,
  ManualPlacementRejectedError,
} from '../../src/xr/manualControlPoints';
import { registerManualPlacementResolver } from '../../src/xr/manualPlacementSurface';

afterEach(() => {
  deactivateCertifiedGuideRegistry();
  registerManualPlacementResolver(null);
});

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

  it('uses the robust physical-surface resolver when the certified worker registry is active but empty', () => {
    setCertifiedGuideCorners([]);
    setCertifiedGuideEdges([]);
    registerManualPlacementResolver((candidate) => ({
      accepted: true,
      position: [candidate[0], candidate[1], candidate[2]],
      mode: 'edge',
      confidence: 0.81,
      supportCount: 18,
      sourceDistanceMeters: 0.01,
      message: 'EDGE LOCK · robust physical two-plane support',
    }));

    const store = new ManualControlPointStore();
    const result = store.addOrSnap([0.3, 0.4, 0.5]);

    expect(result.created).toBe(true);
    expect(result.snapped).toBe(true);
    expect(result.placementMode).toBe('edge');
    expect(result.placementConfidence).toBeCloseTo(0.81, 6);
    expect(result.position).toEqual([0.3, 0.4, 0.5]);
  });

  it('does not turn an empty certified registry into permission to store arbitrary air points', () => {
    setCertifiedGuideCorners([]);
    setCertifiedGuideEdges([]);
    rejectEverything();

    const store = new ManualControlPointStore();
    expect(() => store.addOrSnap([8, 8, 8])).toThrow(ManualPlacementRejectedError);
    expect(store.size).toBe(0);
  });
});
