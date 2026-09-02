import { describe, expect, it } from 'vitest';

import {
  captureStructuralPlanePrior,
  captureStructuralPlanePriors,
  type XrPlaneFrameLike,
  type XrPlaneLike,
} from '../../src/xr/webxrPlaneGuidance';

function plane(overrides: Partial<XrPlaneLike> = {}): XrPlaneLike {
  return {
    planeSpace: {},
    polygon: [
      { x: -0.5, y: 0, z: -0.25 },
      { x: 0.5, y: 0, z: -0.25 },
      { x: 0.5, y: 0, z: 0.25 },
      { x: -0.5, y: 0, z: 0.25 },
    ],
    orientation: 'horizontal',
    semanticLabel: 'floor',
    lastChangedTime: 123,
    ...overrides,
  };
}

function frameFor(planes: readonly XrPlaneLike[], matrix: Float32Array): XrPlaneFrameLike {
  return {
    detectedPlanes: planes,
    getPose: () => ({ transform: { matrix } }),
  };
}

const identity = new Float32Array([
  1,0,0,0,
  0,1,0,0,
  0,0,1,0,
  0,0,0,1,
]);

describe('WebXR native plane guidance capture', () => {
  it('converts plane center, +Y normal and polygon extent into project coordinates', () => {
    const matrix = new Float32Array(identity);
    matrix[12] = 1;
    matrix[13] = 2;
    matrix[14] = 3;
    const source = plane();
    const prior = captureStructuralPlanePrior(source, frameFor([source], matrix), {});

    expect(prior).not.toBeNull();
    expect(prior!.center).toEqual([1, -3, 2]);
    // Reference +Y maps to project +Z.
    expect(prior!.normal[0]).toBeCloseTo(0, 6);
    expect(prior!.normal[1]).toBeCloseTo(0, 6);
    expect(prior!.normal[2]).toBeCloseTo(1, 6);
    expect(prior!.extentRadiusMeters).toBeCloseTo(Math.hypot(0.5, 0.25), 6);
    expect(prior!.areaSquareMeters).toBeCloseTo(0.5, 6);
    expect(prior!.polygonVertexCount).toBe(4);
    expect(prior!.semanticLabel).toBe('floor');
  });

  it('uses the transformed local +Y column as the plane normal', () => {
    // Rotate local +Y onto reference +X.
    const matrix = new Float32Array([
      0,1,0,0,
      1,0,0,0,
      0,0,-1,0,
      0,0,0,1,
    ]);
    const source = plane({ orientation: 'vertical' });
    const prior = captureStructuralPlanePrior(source, frameFor([source], matrix), {});
    expect(prior).not.toBeNull();
    expect(prior!.normal[0]).toBeCloseTo(1, 6);
    expect(prior!.normal[1]).toBeCloseTo(0, 6);
    expect(prior!.normal[2]).toBeCloseTo(0, 6);
  });

  it('drops invalid/degenerate native polygons instead of manufacturing a plane extent', () => {
    const source = plane({ polygon: [{ x: 0, y: 0, z: 0 }] });
    expect(captureStructuralPlanePrior(source, frameFor([source], identity), {})).toBeNull();
  });

  it('bounds a fragmented native plane set by largest polygon area', () => {
    const small = plane({ polygon: [
      { x: -0.05, y: 0, z: -0.05 }, { x: 0.05, y: 0, z: -0.05 },
      { x: 0.05, y: 0, z: 0.05 }, { x: -0.05, y: 0, z: 0.05 },
    ] });
    const medium = plane({ polygon: [
      { x: -0.2, y: 0, z: -0.2 }, { x: 0.2, y: 0, z: -0.2 },
      { x: 0.2, y: 0, z: 0.2 }, { x: -0.2, y: 0, z: 0.2 },
    ] });
    const large = plane();
    const result = captureStructuralPlanePriors(frameFor([small, large, medium], identity), {}, 2);
    expect(result).toHaveLength(2);
    expect(result[0].areaSquareMeters).toBeGreaterThan(result[1].areaSquareMeters);
    expect(result.some((item) => item.areaSquareMeters < 0.02)).toBe(false);
  });

  it('fails advisory when frame-scoped detectedPlanes access throws', () => {
    const frame: XrPlaneFrameLike = {
      get detectedPlanes(): Iterable<XrPlaneLike> { throw new DOMException('inactive', 'InvalidStateError'); },
      getPose: () => ({ transform: { matrix: identity } }),
    };
    expect(captureStructuralPlanePriors(frame, {})).toEqual([]);
  });
});
