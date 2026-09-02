import { describe, expect, it } from 'vitest';

import { stabilizeMeasuredSurfelsWithNativePlanes } from '../../src/xr/nativePlaneSurfelGuidance';
import type { StructuralPlanePrior } from '../../src/xr/webxrPlaneGuidance';

function prior(overrides: Partial<StructuralPlanePrior> = {}): StructuralPlanePrior {
  return {
    center: [0, 0, 0],
    normal: [0, 0, 1],
    tangentU: [1, 0, 0],
    tangentV: [0, 1, 0],
    polygonUv: [[-0.5, -0.5], [0.5, -0.5], [0.5, 0.5], [-0.5, 0.5]],
    extentRadiusMeters: Math.SQRT1_2,
    areaSquareMeters: 1,
    polygonVertexCount: 4,
    orientation: 'vertical',
    semanticLabel: null,
    lastChangedTime: 1,
    ...overrides,
  };
}

describe('native plane surfel guidance', () => {
  it('stabilizes only the normal of a nearby compatible measured surfel', () => {
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
    // WebXR Plane Detection has no numeric confidence. Native metadata must never
    // promote the physical surfel confidence used by occupancy/geometry gates.
    expect(confidence[0]).toBeCloseTo(0.72, 6);
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

  it('rejects a measured surfel outside the actual plane polygon even when it is inside the old radius gate', () => {
    const narrow = prior({
      polygonUv: [[-0.5, -0.08], [0.5, -0.08], [0.5, 0.08], [-0.5, 0.08]],
      extentRadiusMeters: 0.51,
    });
    const positions = new Float32Array([0.1, 0.32, 0.01]);
    const normals = new Float32Array([0.12, 0, 0.993]);
    const confidence = new Float32Array([0.8]);
    const before = Array.from(normals);

    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [narrow])).toBe(0);
    expect(Array.from(normals)).toEqual(before);
  });

  it('allows a small polygon-boundary margin for native tracker jitter', () => {
    const positions = new Float32Array([0.1, 0.54, 0.01]);
    const normals = new Float32Array([0.12, 0, 0.993]);
    const confidence = new Float32Array([0.8]);
    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(1);
  });

  it('never creates points when the physical surfel arrays are empty', () => {
    const positions = new Float32Array(0);
    const normals = new Float32Array(0);
    const confidence = new Float32Array(0);
    expect(stabilizeMeasuredSurfelsWithNativePlanes(positions, normals, confidence, [prior()])).toBe(0);
    expect(positions.length).toBe(0);
  });
});
