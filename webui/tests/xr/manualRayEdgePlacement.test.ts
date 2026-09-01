import { describe, expect, it } from 'vitest';

import { intersectCameraRayWithCertifiedEdge } from '../../src/xr/manualRayEdgePlacement';

const edge = {
  id: 'edge:test',
  start: [0, 0, 1] as const,
  end: [1, 0, 1] as const,
  confidence: 0.9,
  support: 20,
  observations: 4,
  source: 'robust-two-plane' as const,
};

describe('camera ray constrained edge placement', () => {
  it('places a point on the measured edge when the ray crosses it', () => {
    const result = intersectCameraRayWithCertifiedEdge({
      origin: [0.5, -1, 1],
      direction: [0, 1, 0],
    }, edge);
    expect(result.accepted).toBe(true);
    expect(result.position).not.toBeNull();
    expect(result.position![0]).toBeCloseTo(0.5, 5);
    expect(result.position![1]).toBeCloseTo(0, 5);
    expect(result.position![2]).toBeCloseTo(1, 5);
    expect(result.skewDistanceMeters).toBeLessThan(1e-5);
  });

  it('rejects a nearly parallel camera ray', () => {
    const result = intersectCameraRayWithCertifiedEdge({
      origin: [0, -0.2, 1],
      direction: [1, 0.01, 0],
    }, edge);
    expect(result.accepted).toBe(false);
    expect(result.message).toContain('parallel');
  });

  it('rejects a solution beyond the finite measured edge', () => {
    const result = intersectCameraRayWithCertifiedEdge({
      origin: [1.3, -1, 1],
      direction: [0, 1, 0],
    }, edge);
    expect(result.accepted).toBe(false);
    expect(result.message).toContain('outside');
  });

  it('rejects a ray that only passes near the line in 2D but misses it in 3D', () => {
    const result = intersectCameraRayWithCertifiedEdge({
      origin: [0.5, -1, 1.2],
      direction: [0, 1, 0],
    }, edge);
    expect(result.accepted).toBe(false);
    expect(result.skewDistanceMeters).toBeGreaterThan(0.1);
  });
});
