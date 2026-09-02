import { describe, expect, it } from 'vitest';

import {
  CertifiedStructuralCornerTracker,
  detectCertifiedStructuralCorners,
} from '../../src/xr/certifiedStructuralGeometry';
import type { ManualPlacementSurfaceSnapshot } from '../../src/xr/manualPlacementSurface';
import type { StructuralGuideViewpoint } from '../../src/xr/structuralGuide';

interface MutableCloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function add(
  cloud: MutableCloud,
  point: readonly [number, number, number],
  normal: readonly [number, number, number],
): void {
  cloud.positions.push(...point);
  cloud.normals.push(...normal);
  cloud.confidence.push(0.93);
}

function snapshot(cloud: MutableCloud): ManualPlacementSurfaceSnapshot {
  return {
    positions: Float32Array.from(cloud.positions),
    normals: Float32Array.from(cloud.normals),
    confidence: Float32Array.from(cloud.confidence),
    pointCount: cloud.confidence.length,
    voxelSizeMeters: 0.04,
  };
}

function cornerCloud(includeThirdPlane = true): MutableCloud {
  const cloud: MutableCloud = { positions: [], normals: [], confidence: [] };
  for (const y of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(cloud, [0, y, z], [1, 0, 0]);
  }
  for (const x of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(cloud, [x, 0, z], [0, 1, 0]);
  }
  if (includeThirdPlane) {
    for (const x of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
      for (const y of [-0.06, -0.02, 0.02, 0.06]) add(cloud, [x, y, 0], [0, 0, 1]);
    }
  }
  return cloud;
}

function viewpoint(
  keyframeId: number,
  position: readonly [number, number, number],
): StructuralGuideViewpoint {
  return { keyframeId, position };
}

describe('certified structural geometry', () => {
  it('discovers a measured three-plane corner without legacy patch hypotheses', () => {
    const corners = detectCertifiedStructuralCorners(snapshot(cornerCloud(true)));
    expect(corners.length).toBeGreaterThan(0);
    const nearest = corners.reduce((best, candidate) =>
      Math.hypot(...candidate.position) < Math.hypot(...best.position) ? candidate : best,
    );
    expect(Math.hypot(...nearest.position)).toBeLessThan(0.03);
    expect(nearest.confidence).toBeGreaterThanOrEqual(0.72);
    expect(nearest.confidence).toBeLessThan(1);
    // Geometry detection is one current observation. Temporal maturity belongs
    // to CertifiedStructuralCornerTracker and cannot be fabricated here.
    expect(nearest.observations).toBe(1);
  });

  it('does not invent a point corner from only two planes', () => {
    const corners = detectCertifiedStructuralCorners(snapshot(cornerCloud(false)));
    expect(corners).toHaveLength(0);
  });

  it('respects coverageReady evidence when supplied', () => {
    const base = snapshot(cornerCloud(true));
    const blocked = detectCertifiedStructuralCorners({
      ...base,
      coverageReady: new Uint8Array(base.pointCount),
    });
    expect(blocked).toHaveLength(0);
  });
});

describe('certified three-plane candidate-specific tracking', () => {
  it('does not mature a robust corner from one analysis', () => {
    const tracker = new CertifiedStructuralCornerTracker();
    const result = tracker.update(snapshot(cornerCloud(true)), viewpoint(10, [0.5, 0.5, 0.5]));
    expect(result).toHaveLength(0);
  });

  it('does not mature by replaying the same camera viewpoint under newer keyframe ids', () => {
    const tracker = new CertifiedStructuralCornerTracker();
    const source = snapshot(cornerCloud(true));
    tracker.update(source, viewpoint(10, [0.5, 0.5, 0.5]));
    expect(tracker.update(source, viewpoint(14, [0.5, 0.5, 0.5]))).toHaveLength(0);
    expect(tracker.update(source, viewpoint(18, [0.505, 0.5, 0.5]))).toHaveLength(0);
  });

  it('matures the same robust corner only after an independent camera viewpoint', () => {
    const tracker = new CertifiedStructuralCornerTracker();
    const source = snapshot(cornerCloud(true));
    tracker.update(source, viewpoint(10, [0.5, 0.5, 0.5]));
    const result = tracker.update(source, viewpoint(14, [0.28, 0.50, 0.55]));
    expect(result.length).toBeGreaterThan(0);
    expect(result[0].observations).toBe(2);
    expect(result[0].id.startsWith('robust-track:')).toBe(true);
  });

  it('does not mature without a real viewpoint even if robust geometry is repeatedly re-solved', () => {
    const tracker = new CertifiedStructuralCornerTracker();
    const source = snapshot(cornerCloud(true));
    expect(tracker.update(source, null)).toHaveLength(0);
    expect(tracker.update(source, null)).toHaveLength(0);
  });

  it('reset discards candidate-specific maturity', () => {
    const tracker = new CertifiedStructuralCornerTracker();
    const source = snapshot(cornerCloud(true));
    tracker.update(source, viewpoint(10, [0.5, 0.5, 0.5]));
    expect(tracker.update(source, viewpoint(14, [0.28, 0.50, 0.55])).length).toBeGreaterThan(0);
    tracker.reset();
    expect(tracker.update(source, viewpoint(30, [0.15, 0.45, 0.55]))).toHaveLength(0);
  });
});
