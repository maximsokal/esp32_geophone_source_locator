import { describe, expect, it } from 'vitest';

import { stabilizeMeasuredPlaneWithNativePrior } from '../../src/xr/structuralPlanePrior';
import type { StructuralPlanePrior } from '../../src/xr/webxrPlaneGuidance';

function prior(overrides: Partial<StructuralPlanePrior> = {}): StructuralPlanePrior {
  return {
    center: [0, 0, 0],
    normal: [0, 0, 1],
    extentRadiusMeters: 1,
    areaSquareMeters: 2,
    polygonVertexCount: 4,
    orientation: 'vertical',
    semanticLabel: null,
    lastChangedTime: 1,
    ...overrides,
  };
}

describe('native plane prior stabilization', () => {
  it('softly reduces normal jitter on a nearby matching measured plane', () => {
    const measured = {
      center: [0.2, 0.1, 0.015] as const,
      normal: [0.12, 0, Math.sqrt(1 - 0.12 ** 2)] as const,
      confidence: 0.65,
      planarity: 0.7,
      support: 9,
    };
    const stabilized = stabilizeMeasuredPlaneWithNativePrior(measured, [prior()]);
    expect(Math.abs(stabilized.normal[0])).toBeLessThan(Math.abs(measured.normal[0]));
    expect(stabilized.normal[2]).toBeGreaterThan(measured.normal[2]);
    expect(stabilized.confidence).toBeGreaterThan(measured.confidence);
    expect(stabilized.planarity).toBeGreaterThan(measured.planarity);
    expect(stabilized.center).toBe(measured.center);
    expect(stabilized.support).toBe(9);
  });

  it('aligns opposite normal signs without flipping the measured surface', () => {
    const measured = {
      center: [0, 0, 0.01] as const,
      normal: [0, 0, -1] as const,
      confidence: 0.7,
      planarity: 0.8,
    };
    const stabilized = stabilizeMeasuredPlaneWithNativePrior(measured, [prior()]);
    expect(stabilized.normal[2]).toBeLessThan(-0.99);
  });

  it('ignores a parallel native plane that is too far from the measured patch', () => {
    const measured = {
      center: [0, 0, 0.3] as const,
      normal: [0, 0, 1] as const,
      confidence: 0.65,
      planarity: 0.7,
    };
    expect(stabilizeMeasuredPlaneWithNativePrior(measured, [prior()])).toBe(measured);
  });

  it('ignores a native plane with incompatible orientation', () => {
    const measured = {
      center: [0, 0, 0] as const,
      normal: [1, 0, 0] as const,
      confidence: 0.65,
      planarity: 0.7,
    };
    expect(stabilizeMeasuredPlaneWithNativePrior(measured, [prior()])).toBe(measured);
  });

  it('cannot manufacture a plane because an existing measured patch is mandatory input', () => {
    const measured = {
      center: [0, 0, 0] as const,
      normal: [0, 1, 0] as const,
      confidence: 0.4,
      planarity: 0.2,
    };
    const result = stabilizeMeasuredPlaneWithNativePrior(measured, []);
    expect(result).toBe(measured);
    expect(result.confidence).toBe(0.4);
  });
});
