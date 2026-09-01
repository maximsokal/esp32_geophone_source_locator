import { describe, expect, it } from 'vitest';

import { detectCertifiedStructuralCorners } from '../../src/xr/certifiedStructuralGeometry';
import type { ManualPlacementSurfaceSnapshot } from '../../src/xr/manualPlacementSurface';

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
