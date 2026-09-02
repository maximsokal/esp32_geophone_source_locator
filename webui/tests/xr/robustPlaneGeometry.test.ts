import { describe, expect, it } from 'vitest';

import {
  extractRobustPlanes,
  intersectTwoRobustPlanes,
  solveThreeRobustPlanes,
  type RobustPlane,
  type RobustPlaneSample,
} from '../../src/xr/robustPlaneGeometry';

function sample(
  position: readonly [number, number, number],
  normal: readonly [number, number, number],
  weight = 0.9,
): RobustPlaneSample {
  return { position, normal, weight, residualMeters: 0.004, normalConfidence: 0.9 };
}

function threePlanesWithOutliers(): RobustPlaneSample[] {
  const output: RobustPlaneSample[] = [];
  for (const y of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) output.push(sample([0.001 * Math.sin(y * 30), y, z], [1, 0, 0]));
  }
  for (const x of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) output.push(sample([x, 0.001 * Math.cos(x * 30), z], [0, 1, 0]));
  }
  for (const x of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const y of [-0.06, -0.02, 0.02, 0.06]) output.push(sample([x, y, 0.001 * Math.sin((x + y) * 20)], [0, 0, 1]));
  }
  output.push(sample([0.08, 0.08, 0.09], [0.2, 0.6, 0.77], 0.3));
  output.push(sample([-0.09, 0.07, -0.08], [0.7, 0.1, 0.7], 0.25));
  output.push(sample([0.04, -0.09, 0.10], [0.4, 0.8, 0.2], 0.2));
  return output;
}

function axisPlane(
  normal: readonly [number, number, number],
  confidence: number,
): RobustPlane {
  return {
    center: [0, 0, 0],
    normal,
    d: 0,
    support: 12,
    supportWeight: 10,
    rmsResidualMeters: 0.004,
    robustScaleMeters: 0.004,
    tangentSpreadMeters: 0.08,
    offsetSigmaMeters: 0.005,
    confidence,
    inlierIndices: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
  };
}

describe('robust uncertainty-aware plane geometry', () => {
  it('extracts three actual planes despite range outliers', () => {
    const planes = extractRobustPlanes(threePlanesWithOutliers(), {
      maxPlanes: 3,
      minSupport: 8,
      inlierThresholdMeters: 0.012,
    });
    expect(planes).toHaveLength(3);
    for (const plane of planes) {
      expect(plane.support).toBeGreaterThanOrEqual(8);
      expect(plane.rmsResidualMeters).toBeLessThan(0.008);
      expect(plane.offsetSigmaMeters).toBeLessThan(0.008);
      expect(plane.confidence).toBeGreaterThan(0.6);
    }
  });

  it('reports a bounded uncertainty for a strong two-plane edge', () => {
    const planes = extractRobustPlanes(threePlanesWithOutliers(), { maxPlanes: 3, minSupport: 8 });
    const edge = intersectTwoRobustPlanes(planes[0], planes[1], [0.02, -0.01, 0.03]);
    expect(edge).not.toBeNull();
    expect(edge!.uncertaintyMeters).toBeLessThan(0.02);
  });

  it('solves a well-conditioned three-plane corner near the measured origin', () => {
    const planes = extractRobustPlanes(threePlanesWithOutliers(), { maxPlanes: 3, minSupport: 8 });
    const corner = solveThreeRobustPlanes(planes[0], planes[1], planes[2]);
    expect(corner).not.toBeNull();
    expect(corner!.conditionNumber).toBeLessThan(10);
    expect(corner!.maxStdDevMeters).toBeLessThan(0.02);
    expect(Math.hypot(...corner!.position)).toBeLessThan(0.015);
  });

  it('propagates weak plane confidence into corner covariance instead of treating it as confidence one', () => {
    const x = axisPlane([1, 0, 0], 0.95);
    const y = axisPlane([0, 1, 0], 0.95);
    const strongZ = axisPlane([0, 0, 1], 0.95);
    const weakZ = axisPlane([0, 0, 1], 0.20);

    const strong = solveThreeRobustPlanes(x, y, strongZ);
    const weak = solveThreeRobustPlanes(x, y, weakZ);
    expect(strong).not.toBeNull();
    expect(weak).not.toBeNull();
    // For three independent exact equations the intersection itself is the same;
    // plane confidence belongs in the information/covariance, not as a fake
    // coordinate displacement. The weak axis must therefore be measurably less certain.
    expect(Math.hypot(...weak!.position)).toBeCloseTo(Math.hypot(...strong!.position), 8);
    expect(weak!.maxStdDevMeters).toBeGreaterThan(strong!.maxStdDevMeters * 1.8);
    expect(weak!.confidence).toBeLessThan(strong!.confidence);
  });

  it('rejects a nearly parallel plane system instead of inventing a distant corner', () => {
    const base = extractRobustPlanes(threePlanesWithOutliers(), { maxPlanes: 3, minSupport: 8 });
    expect(base.length).toBe(3);
    const badThird = {
      ...base[0],
      normal: [0.9986, 0.0523, 0] as const,
      d: 0.02,
    };
    expect(solveThreeRobustPlanes(base[0], base[1], badThird)).toBeNull();
  });
});
