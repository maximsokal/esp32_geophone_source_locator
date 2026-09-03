import { describe, expect, it } from 'vitest';

import {
  estimateSurfaceFeatureComplexity,
  estimateSurfaceProbeComplexity,
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

function positionedNormal(
  angleDegrees: number,
  x: number,
  y: number,
  z = 0,
  confidence = 0.9,
) {
  return { ...normal(angleDegrees, confidence), x, y, z };
}

function maturePositionedNormal(
  angleDegrees: number,
  x: number,
  y: number,
  z = 0,
  confidence = 0.9,
) {
  return {
    ...positionedNormal(angleDegrees, x, y, z, confidence),
    promoted: true,
    independentViews: 2,
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

  it('keeps a spatially extended convex surface feature-rich with metric positions', () => {
    const feature = estimateSurfaceFeatureComplexity(
      [-28, -20, -12, -4, 4, 12, 20, 28].map((angle, index) =>
        positionedNormal(angle, index * 0.025, 0)
      ),
    );
    expect(feature.spatialSpanMeters).toBeGreaterThan(0.15);
    expect(feature.score).toBeGreaterThan(0.25);
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

  it('lets tentative measured curvature request probes without granting retention density', () => {
    const samples = [-28, -20, -12, -4, 4, 12, 20, 28].map((angle, index) => ({
      ...positionedNormal(angle, index * 0.025, 0),
      promoted: false,
      independentViews: 1,
    }));
    expect(estimateSurfaceFeatureComplexity(samples).score).toBe(0);
    expect(estimateSurfaceProbeComplexity(samples).score).toBeGreaterThan(0.25);
    expect(featureAwareCellQuota(28, estimateSurfaceFeatureComplexity(samples).score)).toBe(28);
  });

  it('keeps probe-only classification robust to one abnormal normal on a flat patch', () => {
    const feature = estimateSurfaceProbeComplexity([
      positionedNormal(-2, 0.00, 0), positionedNormal(-1, 0.02, 0),
      positionedNormal(0, 0.04, 0), positionedNormal(1, 0.06, 0),
      positionedNormal(2, 0.08, 0), positionedNormal(90, 0.10, 0),
    ]);
    expect(feature.score).toBeLessThan(0.20);
  });

  it('does not let tentative conflicting surfels trigger feature density', () => {
    const feature = estimateSurfaceFeatureComplexity([
      { ...positionedNormal(0, 0.00, 0.00), promoted: false, independentViews: 1 },
      { ...positionedNormal(1, 0.03, 0.00), promoted: false, independentViews: 1 },
      { ...positionedNormal(-1, 0.06, 0.00), promoted: false, independentViews: 1 },
      { ...positionedNormal(88, 0.00, 0.01), promoted: false, independentViews: 1 },
      { ...positionedNormal(90, 0.00, 0.04), promoted: false, independentViews: 1 },
      { ...positionedNormal(92, 0.00, 0.07), promoted: false, independentViews: 1 },
    ]);
    expect(feature.usableNormals).toBe(0);
    expect(feature.matureOrientationFamilies).toBe(0);
    expect(feature.score).toBe(0);
    expect(featureAwareCellQuota(28, feature.score)).toBe(28);
  });

  it('does not treat a nominally promoted single-view sample as mature feature evidence', () => {
    const feature = estimateSurfaceFeatureComplexity([
      { ...positionedNormal(0, 0.00, 0.00), promoted: true, independentViews: 1 },
      { ...positionedNormal(90, 0.00, 0.06), promoted: true, independentViews: 1 },
    ]);
    expect(feature.usableNormals).toBe(0);
    expect(feature.score).toBe(0);
  });

  it('restores feature density once the same geometry has independent physical support', () => {
    const feature = estimateSurfaceFeatureComplexity([
      maturePositionedNormal(0, 0.00, 0.00),
      maturePositionedNormal(1, 0.03, 0.00),
      maturePositionedNormal(-1, 0.06, 0.00),
      maturePositionedNormal(88, 0.00, 0.01),
      maturePositionedNormal(90, 0.00, 0.04),
      maturePositionedNormal(92, 0.00, 0.07),
    ]);
    expect(feature.usableNormals).toBe(6);
    expect(feature.spatiallySupportedFamilies).toBeGreaterThanOrEqual(2);
    expect(feature.score).toBeGreaterThan(0.55);
    expect(featureAwareCellQuota(28, feature.score)).toBeGreaterThan(34);
  });

  it('does not give a hard-feature density bonus to conflicting normals packed into one tiny spot', () => {
    const feature = estimateSurfaceFeatureComplexity([
      positionedNormal(0, 0.000, 0.000),
      positionedNormal(1, 0.004, 0.000),
      positionedNormal(-1, 0.007, 0.002),
      positionedNormal(88, 0.001, 0.003),
      positionedNormal(90, 0.005, 0.004),
      positionedNormal(92, 0.008, 0.005),
    ]);
    expect(feature.matureOrientationFamilies).toBeGreaterThanOrEqual(2);
    expect(feature.spatiallySupportedFamilies).toBeLessThan(2);
    expect(feature.spatialSpanMeters).toBeLessThan(0.02);
    expect(feature.score).toBeLessThan(0.15);
  });

  it('restores a strong hard-feature score once both surfaces have real metric support', () => {
    const feature = estimateSurfaceFeatureComplexity([
      positionedNormal(0, 0.00, 0.00),
      positionedNormal(1, 0.03, 0.00),
      positionedNormal(-1, 0.06, 0.00),
      positionedNormal(88, 0.00, 0.01),
      positionedNormal(90, 0.00, 0.04),
      positionedNormal(92, 0.00, 0.07),
    ]);
    expect(feature.spatiallySupportedFamilies).toBeGreaterThanOrEqual(2);
    expect(feature.spatialSpanMeters).toBeGreaterThan(0.07);
    expect(feature.score).toBeGreaterThan(0.55);
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

  it('is spatially invariant to input permutation when positions are supplied', () => {
    const ordered = [
      positionedNormal(0, 0.00, 0.00, 0, 0.9),
      positionedNormal(1, 0.03, 0.00, 0, 0.9),
      positionedNormal(90, 0.00, 0.02, 0, 0.9),
      positionedNormal(91, 0.00, 0.06, 0, 0.9),
    ];
    const a = estimateSurfaceFeatureComplexity(ordered);
    const b = estimateSurfaceFeatureComplexity([ordered[3], ordered[0], ordered[2], ordered[1]]);
    expect(b.score).toBeCloseTo(a.score, 10);
    expect(b.spatialSpanMeters).toBeCloseTo(a.spatialSpanMeters, 10);
    expect(b.spatiallySupportedFamilies).toBe(a.spatiallySupportedFamilies);
  });

  it('scales the feature bonus down with global memory pressure and respects the cap', () => {
    expect(featureAwareCellQuota(8, 1)).toBe(14);
    expect(featureAwareCellQuota(28, 1)).toBe(48);
    expect(featureAwareCellQuota(40, 1, 48)).toBe(48);
  });
});
