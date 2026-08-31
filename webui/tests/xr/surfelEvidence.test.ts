import { describe, expect, it } from 'vitest';
import {
  classifyDepthNeighbourhood,
  packInitialView,
  registerIndependentView,
  stratifiedSampleCoordinate,
  surfaceRenderConfidence,
} from '../../src/xr/surfelEvidence';

function direction(angleDegrees: number): readonly [number, number, number] {
  const angle = (angleDegrees * Math.PI) / 180;
  return [Math.sin(angle), 0, Math.cos(angle)];
}

describe('surfel view evidence', () => {
  it('does not turn repeated near-identical global keyframes into independent views', () => {
    const initial = packInitialView(direction(0));
    for (let index = 0; index < 20; index += 1) {
      const update = registerIndependentView(initial.packed, initial.count, direction(2));
      expect(update.independent).toBe(false);
      expect(update.count).toBe(1);
    }
  });

  it('accepts a genuinely different view and rejects repeating that same view', () => {
    const initial = packInitialView(direction(0));
    const different = registerIndependentView(initial.packed, initial.count, direction(14));
    expect(different.independent).toBe(true);
    expect(different.count).toBe(2);

    const repeated = registerIndependentView(different.packed, different.count, direction(14));
    expect(repeated.independent).toBe(false);
    expect(repeated.count).toBe(2);
  });
});

describe('edge-aware depth quality', () => {
  it('keeps a true depth discontinuity when the center is supported on its own layer', () => {
    const quality = classifyDepthNeighbourhood(1.0, 1.01, 2.0, 0.99, 2.02);
    expect(quality.accepted).toBe(true);
    expect(quality.edge).toBe(true);
    expect(quality.supportingNeighbours).toBe(2);
    expect(quality.score).toBeGreaterThan(0.5);
  });

  it('keeps a one-sided silhouette sample instead of median-filtering the edge away', () => {
    const quality = classifyDepthNeighbourhood(1.0, 1.01, 2.0, 2.0, 2.0);
    expect(quality.accepted).toBe(true);
    expect(quality.edge).toBe(true);
    expect(quality.supportingNeighbours).toBe(1);
  });

  it('rejects an isolated depth spike', () => {
    const quality = classifyDepthNeighbourhood(1.5, 1.0, 1.0, 1.0, 1.0);
    expect(quality.accepted).toBe(false);
  });
});

describe('sampling and display confidence', () => {
  it('moves samples inside each stratum instead of stamping a permanent regular lattice', () => {
    const first = stratifiedSampleCoordinate(10, 40, 0.025, 1, 1);
    const second = stratifiedSampleCoordinate(10, 40, 0.025, 2, 1);
    expect(first).not.toBe(second);
    expect(first).toBeGreaterThan(0.025);
    expect(first).toBeLessThan(0.975);
    expect(second).toBeGreaterThan(0.025);
    expect(second).toBeLessThan(0.975);
  });

  it('never paints an unpromoted hypothesis as high-confidence green', () => {
    const tentative = surfaceRenderConfidence({
      promoted: false,
      independentViews: 1,
      weight: 12,
      residualMeters: 0,
      maxViewAngleRadians: 0,
    });
    const strong = surfaceRenderConfidence({
      promoted: true,
      independentViews: 3,
      weight: 6,
      residualMeters: 0.008,
      maxViewAngleRadians: (22 * Math.PI) / 180,
    });
    expect(tentative).toBeLessThanOrEqual(0.48);
    expect(strong).toBeGreaterThan(0.8);
    expect(strong).toBeGreaterThan(tentative);
  });
});
