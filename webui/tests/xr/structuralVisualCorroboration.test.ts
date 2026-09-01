import { describe, expect, it } from 'vitest';

import { applyStructuralVisualCorroboration } from '../../src/xr/structuralVisualCorroboration';
import type { ReprojectionCertificate } from '../../src/xr/reprojectionCertificate';

function certificate(overrides: Partial<ReprojectionCertificate> = {}): ReprojectionCertificate {
  return {
    accepted: true,
    conclusive: true,
    observations: 3,
    eligibleObservations: 3,
    meanErrorNormalized: 0.01,
    confidence: 0.8,
    matchedKeyframes: [1, 2, 3],
    eligibleKeyframes: [1, 2, 3],
    ...overrides,
  };
}

describe('structural RGB corroboration policy', () => {
  it('keeps robust metric geometry when RGB evidence is inconclusive', () => {
    const result = applyStructuralVisualCorroboration(0.82, 6, certificate({
      accepted: true,
      conclusive: false,
      observations: 1,
      eligibleObservations: 1,
    }));
    expect(result.confidence).toBeCloseTo(0.82, 6);
    expect(result.observations).toBe(6);
    expect(result.visuallyConfirmed).toBe(false);
    expect(result.visualDisagreement).toBe(false);
  });

  it('does not let a missed Hough match erase independently mature 3D geometry', () => {
    const result = applyStructuralVisualCorroboration(0.84, 7, certificate({
      accepted: false,
      observations: 1,
      eligibleObservations: 4,
      confidence: 0.2,
    }));
    expect(result.confidence).toBeCloseTo(0.84, 6);
    expect(result.observations).toBe(7);
    expect(result.visuallyConfirmed).toBe(false);
    expect(result.visualDisagreement).toBe(true);
  });

  it('uses positive visual reprojection only as a small corroborating bonus', () => {
    const result = applyStructuralVisualCorroboration(0.70, 3, certificate({ confidence: 0.9, observations: 5 }));
    expect(result.confidence).toBeGreaterThan(0.70);
    expect(result.confidence).toBeLessThan(0.73);
    expect(result.observations).toBe(5);
    expect(result.visuallyConfirmed).toBe(true);
  });

  it('never lowers an already strong physical confidence', () => {
    const result = applyStructuralVisualCorroboration(0.91, 7, certificate({ confidence: 0.35 }));
    expect(result.confidence).toBeGreaterThanOrEqual(0.91);
    expect(result.confidence).toBeLessThanOrEqual(0.97);
  });
});
