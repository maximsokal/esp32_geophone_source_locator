import { describe, expect, it } from 'vitest';

import { stabilizeMeasuredSurfelsWithNativePlanes } from '../../src/xr/nativePlaneSurfelGuidance';
import type { StructuralPlanePrior } from '../../src/xr/webxrPlaneGuidance';

function prior(overrides: Partial<StructuralPlanePrior> = {}): StructuralPlanePrior {
  return {
    center: [0, 0, 0],
    normal: [0, 0, 1],
    extentRadiusMeters: 1,
    areaSquareMeters: 1,
    polygonVertexCount: 4,
    orientation: 'vertical',
    semanticLabel: null,
    lastChangedTime: 1,
    ...overrides,
  };
}

describe('native plane surfel guidance', () => {
  it('stabilizes only the normal/confidence of a nearby compatible measured surfel', () => {
    const positions = new Float32Array([0.2, 0.1, 0.015]);
    const x = 0.16;
    const normals = new Float32Array([x, 0, Math.sqrt(1 - x * x)]);
    const confidence = new Float32Array([0.72]);
    const originalPosition = Array.from(positions);
    const originalX = normals[0];

    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(1);
    expect(Array.from(positions)).toEqual(originalPosition);
    expect(Math.abs(normals[0])).toBeLessThan(Math.abs(originalX));
    expect(normals[2]).toBeGreaterThan(0.98);
    expect(confidence[0]).toBeGreaterThan(0.72);
    expect(confidence[0]).toBeLessThanOrEqual(0.745);
  });

  it('does not touch a surfel on a distant parallel surface', () => {
    const positions = new Float32Array([0, 0, 0.3]);
    const normals = new Float32Array([0, 0, 1]);
    const confidence = new Float32Array([0.8]);
    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(0);
    expect(Array.from(normals)).toEqual([0, 0, 1]);
    expect(confidence[0]).toBeCloseTo(0.8, 6);
  });

  it('does not let an orthogonal native plane rewrite measured normals', () => {
    const positions = new Float32Array([0, 0, 0]);
    const normals = new Float32Array([1, 0, 0]);
    const confidence = new Float32Array([0.8]);
    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(0);
    expect(Array.from(normals)).toEqual([1, 0, 0]);
  });

  it('ignores weak measured surfels rather than promoting them using native plane metadata', () => {
    const positions = new Float32Array([0, 0, 0.01]);
    const normals = new Float32Array([0.1, 0, 0.995]);
    const confidence = new Float32Array([0.3]);
    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(0);
    expect(confidence[0]).toBeCloseTo(0.3, 6);
  });

  it('never creates points when the physical surfel arrays are empty', () => {
    const positions = new Float32Array(0);
    const normals = new Float32Array(0);
    const confidence = new Float32Array(0);
    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(0);
    expect(positions.length).toBe(0);
  });
});
