import { describe, expect, it } from 'vitest';
import {
  angularDistance,
  classifyDepthNeighbourhood,
  crossVector,
  decodeViewDirection,
  depthToleranceMeters,
  dotVector,
  encodeViewDirection,
  lowDiscrepancySample2D,
  normalizeVector,
  normalsCompatible,
  orientNormalToward,
  packInitialView,
  registerIndependentView,
  surfaceRenderConfidence,
} from '../../src/xr/surfelEvidence';

function direction(angleDegrees: number): readonly [number, number, number] {
  const angle = (angleDegrees * Math.PI) / 180;
  return [Math.sin(angle), 0, Math.cos(angle)];
}

describe('vector and normal geometry', () => {
  it('normalizes finite vectors', () => {
    expect(normalizeVector([3, 0, 4])[0]).toBeCloseTo(0.6, 6);
    expect(normalizeVector([3, 0, 4])[2]).toBeCloseTo(0.8, 6);
  });

  it('uses a deterministic finite fallback for a zero vector', () => {
    expect(normalizeVector([0, 0, 0])).toEqual([0, 0, 1]);
  });

  it('computes angular distance independently of vector magnitude', () => {
    expect(angularDistance([10, 0, 0], [0, 0, 4])).toBeCloseTo(Math.PI / 2, 6);
  });

  it('cross product produces an orthogonal unit direction', () => {
    const normal = crossVector([1, 0, 0], [0, 1, 0]);
    expect(normal[2]).toBeCloseTo(1, 6);
    expect(dotVector(normal, [1, 0, 0])).toBeCloseTo(0, 6);
  });

  it('orients a normal toward the camera side', () => {
    const normal = orientNormalToward([0, 0, -1], [0, 0, 1]);
    expect(normal[0]).toBeCloseTo(0, 8);
    expect(normal[1]).toBeCloseTo(0, 8);
    expect(normal[2]).toBeCloseTo(1, 8);
  });

  it('accepts same-surface normals but rejects an orthogonal corner', () => {
    expect(normalsCompatible([0, 0, 1], direction(35))).toBe(true);
    expect(normalsCompatible([0, 0, 1], [1, 0, 0])).toBe(false);
  });
});

describe('surfel view evidence', () => {
  it('round-trips quantized view direction with bounded angular error', () => {
    for (const angle of [-70, -30, 0, 25, 70]) {
      const source = direction(angle);
      const decoded = decodeViewDirection(encodeViewDirection(source));
      expect(angularDistance(source, decoded)).toBeLessThan((7 * Math.PI) / 180);
    }
  });

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

  it('does not store more than four independent view codes', () => {
    let state = packInitialView(direction(-40));
    for (const angle of [-15, 10, 35, 60, 80]) {
      const update = registerIndependentView(state.packed, state.count, direction(angle));
      state = { packed: update.packed, count: update.count };
    }
    expect(state.count).toBe(4);
  });

  it('respects a stricter custom angular threshold', () => {
    const initial = packInitialView(direction(0));
    expect(registerIndependentView(initial.packed, 1, direction(15), (20 * Math.PI) / 180).independent).toBe(false);
  });
});

describe('edge-aware depth quality', () => {
  it('increases tolerance with range while remaining capped', () => {
    expect(depthToleranceMeters(4)).toBeGreaterThan(depthToleranceMeters(1));
    expect(depthToleranceMeters(100)).toBeLessThanOrEqual(0.105);
  });

  it('keeps a smooth local surface with high score', () => {
    const quality = classifyDepthNeighbourhood(1.5, 1.502, 1.498, 1.501, 1.499);
    expect(quality.accepted).toBe(true);
    expect(quality.edge).toBe(false);
    expect(quality.score).toBeGreaterThan(0.9);
    expect(quality.supportingNeighbours).toBe(4);
  });

  it('keeps a true depth discontinuity only as weak boundary evidence', () => {
    const quality = classifyDepthNeighbourhood(1.0, 1.01, 2.0, 0.99, 2.02);
    expect(quality.accepted).toBe(true);
    expect(quality.edge).toBe(true);
    expect(quality.supportingNeighbours).toBe(2);
    // Mixed foreground/background pixels can be confidently wrong. Preserve
    // the boundary sample, but never let it carry interior-plane weight.
    expect(quality.score).toBeGreaterThan(0.15);
    expect(quality.score).toBeLessThan(0.5);
  });

  it('identifies the dominant horizontal depth jump', () => {
    expect(classifyDepthNeighbourhood(1.0, 1.0, 2.0, 1.0, 1.0).dominantAxis).toBe('u');
  });

  it('identifies the dominant vertical depth jump', () => {
    expect(classifyDepthNeighbourhood(1.0, 1.0, 1.0, 1.0, 2.0).dominantAxis).toBe('v');
  });

  it('keeps a one-sided silhouette sample with very low weight instead of erasing the edge', () => {
    const quality = classifyDepthNeighbourhood(1.0, 1.01, 2.0, 2.0, 2.0);
    expect(quality.accepted).toBe(true);
    expect(quality.edge).toBe(true);
    expect(quality.supportingNeighbours).toBe(1);
    expect(quality.score).toBeLessThan(0.25);
  });

  it('rejects an isolated depth spike', () => {
    expect(classifyDepthNeighbourhood(1.5, 1.0, 1.0, 1.0, 1.0).accepted).toBe(false);
  });

  it('handles sparse neighbourhoods conservatively but does not discard them outright', () => {
    const quality = classifyDepthNeighbourhood(1.5, 1.5, null, null, null);
    expect(quality.accepted).toBe(true);
    expect(quality.score).toBeLessThan(0.5);
  });

  it('rejects invalid center depths', () => {
    expect(classifyDepthNeighbourhood(Number.NaN, 1, 1, 1, 1).accepted).toBe(false);
    expect(classifyDepthNeighbourhood(-1, 1, 1, 1, 1).accepted).toBe(false);
  });
});

describe('low-discrepancy sampling', () => {
  it('keeps all R2 samples inside the requested image margin', () => {
    for (let index = 0; index < 2000; index += 1) {
      const [u, v] = lowDiscrepancySample2D(index, 0.025, 7);
      expect(u).toBeGreaterThanOrEqual(0.025);
      expect(u).toBeLessThanOrEqual(0.975);
      expect(v).toBeGreaterThanOrEqual(0.025);
      expect(v).toBeLessThanOrEqual(0.975);
    }
  });

  it('does not stamp 880 samples onto only 40 fixed columns / 22 fixed rows', () => {
    const u = new Set<number>();
    const v = new Set<number>();
    for (let index = 0; index < 880; index += 1) {
      const sample = lowDiscrepancySample2D(index, 0.025, 11);
      u.add(Math.round(sample[0] * 1_000_000));
      v.add(Math.round(sample[1] * 1_000_000));
    }
    expect(u.size).toBeGreaterThan(800);
    expect(v.size).toBeGreaterThan(800);
  });

  it('rotates the sequence between integration phases', () => {
    expect(lowDiscrepancySample2D(100, 0.025, 1)).not.toEqual(lowDiscrepancySample2D(100, 0.025, 2));
  });

  it('covers every coarse quadrant instead of clustering', () => {
    const quadrants = [0, 0, 0, 0];
    for (let index = 0; index < 400; index += 1) {
      const [u, v] = lowDiscrepancySample2D(index, 0, 31);
      quadrants[(u >= 0.5 ? 1 : 0) + (v >= 0.5 ? 2 : 0)] += 1;
    }
    quadrants.forEach((count) => expect(count).toBeGreaterThan(75));
  });
});

describe('display confidence semantics', () => {
  it('never paints an unpromoted hypothesis as high-confidence green', () => {
    const tentative = surfaceRenderConfidence({
      promoted: false, independentViews: 1, weight: 12, residualMeters: 0,
      maxViewAngleRadians: 0, normalConfidence: 1,
    });
    expect(tentative).toBeLessThanOrEqual(0.46);
  });

  it('assigns strong multi-view low-residual surfels high confidence', () => {
    const strong = surfaceRenderConfidence({
      promoted: true, independentViews: 3, weight: 6, residualMeters: 0.008,
      maxViewAngleRadians: (22 * Math.PI) / 180, normalConfidence: 0.9,
    });
    expect(strong).toBeGreaterThan(0.8);
  });

  it('penalizes contradictions even after promotion', () => {
    const clean = surfaceRenderConfidence({
      promoted: true, independentViews: 3, weight: 6, residualMeters: 0.008,
      maxViewAngleRadians: (22 * Math.PI) / 180, contradictionWeight: 0,
    });
    const contradicted = surfaceRenderConfidence({
      promoted: true, independentViews: 3, weight: 6, residualMeters: 0.008,
      maxViewAngleRadians: (22 * Math.PI) / 180, contradictionWeight: 3,
    });
    expect(contradicted).toBeLessThan(clean - 0.2);
  });

  it('penalizes poor normal support', () => {
    const weakNormal = surfaceRenderConfidence({
      promoted: true, independentViews: 3, weight: 4, residualMeters: 0.01,
      maxViewAngleRadians: 0.3, normalConfidence: 0,
    });
    const strongNormal = surfaceRenderConfidence({
      promoted: true, independentViews: 3, weight: 4, residualMeters: 0.01,
      maxViewAngleRadians: 0.3, normalConfidence: 1,
    });
    expect(strongNormal).toBeGreaterThan(weakNormal);
  });
});
