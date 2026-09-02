import { describe, expect, it } from 'vitest';

import {
  estimateSurfaceFeatureComplexity,
  featureAwareCellQuota,
} from '../../src/xr/surfaceFeatureDensity';

function normal(angleDegrees: number, confidence = 0.9) {
  const angle = angleDegrees * Math.PI / 180;
  return {
    normalX: Math.sin(angle),
    normalY: 0,
    normalZ: Math.cos(angle),
    normalConfidence: confidence,
  };
}

describe('surface feature complexity', () => {
  it('keeps a slightly noisy flat plane at the base density', () => {
    const feature = estimateSurfaceFeatureComplexity([-3, -2, 0, 1, 2, 3].map((angle) => normal(angle)));
    expect(feature.matureOrientationFamilies).toBe(1);
    expect(feature.score).toBeLessThan(0.12);
    expect(featureAwareCellQuota(28, feature.score)).toBeLessThanOrEqual(30);
  });

  it('treats opposite normal signs as the same physical plane', () => {
    const samples = [normal(0), normal(2), normal(180), normal(178)];
    const feature = estimateSurfaceFeatureComplexity(samples);
    expect(feature.matureOrientationFamilies).toBe(1);
    expect(feature.score).toBeLessThan(0.12);
  });

  it('spends more representatives on a smoothly convex surface', () => {
    const feature = estimateSurfaceFeatureComplexity([-28, -20, -12, -4, 4, 12, 20, 28].map((angle) => normal(angle)));
    expect(feature.robustSpreadRadians).toBeGreaterThan(12 * Math.PI / 180);
    expect(feature.score).toBeGreaterThan(0.25);
    expect(featureAwareCellQuota(28, feature.score)).toBeGreaterThan(32);
  });

  it('gives a strong density bonus to a supported two-plane corner', () => {
    const feature = estimateSurfaceFeatureComplexity([
      normal(0), normal(1), normal(-1),
      normal(88), normal(90), normal(92),
    ]);
    expect(feature.matureOrientationFamilies).toBeGreaterThanOrEqual(2);
    expect(feature.strongestFamilySeparationRadians).toBeGreaterThan(70 * Math.PI / 180);
    expect(feature.score).toBeGreaterThan(0.7);
    expect(featureAwareCellQuota(28, feature.score)).toBeGreaterThanOrEqual(42);
  });

  it('does not let one outlier normal turn a flat cell into a feature cell', () => {
    const feature = estimateSurfaceFeatureComplexity([
      normal(-2), normal(-1), normal(0), normal(1), normal(2), normal(90),
    ]);
    expect(feature.matureOrientationFamilies).toBe(1);
    expect(feature.score).toBeLessThan(0.2);
  });

  it('is invariant to surfel arrival order and normal sign flips', () => {
    const ordered = [
      normal(-18, 0.78), normal(-8, 0.91), normal(2, 0.83),
      normal(74, 0.87), normal(88, 0.93), normal(96, 0.81),
    ];
    const shuffledAndFlipped = [
      normal(276, 0.81), normal(182, 0.83), normal(254, 0.87),
      normal(172, 0.91), normal(162, 0.78), normal(268, 0.93),
    ];
    const a = estimateSurfaceFeatureComplexity(ordered);
    const b = estimateSurfaceFeatureComplexity(shuffledAndFlipped);

    expect(b.matureOrientationFamilies).toBe(a.matureOrientationFamilies);
    expect(b.usableNormals).toBe(a.usableNormals);
    expect(b.score).toBeCloseTo(a.score, 10);
    expect(b.robustSpreadRadians).toBeCloseTo(a.robustSpreadRadians, 10);
    expect(b.strongestFamilySeparationRadians).toBeCloseTo(a.strongestFamilySeparationRadians, 10);
  });

  it('scales the feature bonus down with global memory pressure and respects the cap', () => {
    expect(featureAwareCellQuota(8, 1)).toBe(14);
    expect(featureAwareCellQuota(28, 1)).toBe(48);
    expect(featureAwareCellQuota(40, 1, 48)).toBe(48);
  });
});
