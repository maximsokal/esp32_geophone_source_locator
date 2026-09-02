import { beforeEach, describe, expect, it } from 'vitest';

import type { XrPlaneFrameLike, XrPlaneLike } from '../../src/xr/webxrPlaneGuidance';
import {
  captureLatestStructuralPlanePriors,
  getLatestStructuralPlanePriorCount,
  getLatestStructuralPlanePriors,
  resetXrPlaneGuidanceRegistry,
} from '../../src/xr/xrPlaneGuidanceRegistry';

const identity = new Float32Array([
  1,0,0,0,
  0,1,0,0,
  0,0,1,0,
  0,0,0,1,
]);

function plane(): XrPlaneLike {
  return {
    planeSpace: {},
    polygon: [
      { x: -0.4, y: 0, z: -0.2 },
      { x: 0.4, y: 0, z: -0.2 },
      { x: 0.4, y: 0, z: 0.2 },
      { x: -0.4, y: 0, z: 0.2 },
    ],
    orientation: 'horizontal',
    semanticLabel: 'floor',
    lastChangedTime: 42,
  };
}

function frameFor(planes: readonly XrPlaneLike[]): XrPlaneFrameLike {
  return {
    detectedPlanes: planes,
    getPose: () => ({ transform: { matrix: identity } }),
  };
}

describe('XR plane guidance registry', () => {
  beforeEach(() => resetXrPlaneGuidanceRegistry());

  it('copies frame-scoped planes into owned numeric snapshots', () => {
    const source = plane();
    expect(captureLatestStructuralPlanePriors(frameFor([source]), {})).toBe(1);
    expect(getLatestStructuralPlanePriorCount()).toBe(1);

    const first = getLatestStructuralPlanePriors();
    expect(first).toHaveLength(1);
    expect(first[0].center).toEqual([0, 0, 0]);
    expect(first[0].polygonUv).toEqual([[-0.4, -0.2], [0.4, -0.2], [0.4, 0.2], [-0.4, 0.2]]);

    // Returned snapshots are deep clones: consumer mutation cannot poison later worker analyses.
    (first[0].center as unknown as number[])[0] = 99;
    (first[0].polygonUv[0] as unknown as number[])[0] = 99;
    const second = getLatestStructuralPlanePriors();
    expect(second[0].center[0]).toBe(0);
    expect(second[0].polygonUv[0][0]).toBeCloseTo(-0.4, 6);
  });

  it('clears stale priors when Plane Detection is unsupported for the current frame', () => {
    captureLatestStructuralPlanePriors(frameFor([plane()]), {});
    expect(getLatestStructuralPlanePriorCount()).toBe(1);

    expect(captureLatestStructuralPlanePriors({}, {})).toBe(0);
    expect(getLatestStructuralPlanePriorCount()).toBe(0);
  });

  it('turns inactive-frame access failures into an empty advisory set', () => {
    const frame: XrPlaneFrameLike = {
      get detectedPlanes(): Iterable<XrPlaneLike> { throw new DOMException('inactive', 'InvalidStateError'); },
      getPose: () => ({ transform: { matrix: identity } }),
    };
    expect(captureLatestStructuralPlanePriors(frame, {})).toBe(0);
    expect(getLatestStructuralPlanePriors()).toEqual([]);
  });
});
