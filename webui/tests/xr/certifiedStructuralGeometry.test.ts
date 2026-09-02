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
  confidence = 0.93,
): void {
  cloud.positions.push(...point);
  cloud.normals.push(...normal);
  cloud.confidence.push(confidence);
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

function reversedSnapshot(cloud: MutableCloud): ManualPlacementSurfaceSnapshot {
  const reversed: MutableCloud = { positions: [], normals: [], confidence: [] };
  for (let index = cloud.confidence.length - 1; index >= 0; index -= 1) {
    const offset = index * 3;
    add(
      reversed,
      [cloud.positions[offset], cloud.positions[offset + 1], cloud.positions[offset + 2]],
      [cloud.normals[offset], cloud.normals[offset + 1], cloud.normals[offset + 2]],
      cloud.confidence[index],
    );
  }
  return snapshot(reversed);
}

function appendCorner(
  cloud: MutableCloud,
  origin: readonly [number, number, number] = [0, 0, 0],
  includeThirdPlane = true,
  confidence = 0.93,
): void {
  const [ox, oy, oz] = origin;
  for (const y of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(cloud, [ox, oy + y, oz + z], [1, 0, 0], confidence);
  }
  for (const x of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(cloud, [ox + x, oy, oz + z], [0, 1, 0], confidence);
  }
  if (includeThirdPlane) {
    for (const x of [-0.10, -0.06, -0.02, 0.02, 0.06, 0.10]) {
      for (const y of [-0.06, -0.02, 0.02, 0.06]) add(cloud, [ox + x, oy + y, oz], [0, 0, 1], confidence);
    }
  }
}

function cornerCloud(includeThirdPlane = true): MutableCloud {
  const cloud: MutableCloud = { positions: [], normals: [], confidence: [] };
  appendCorner(cloud, [0, 0, 0], includeThirdPlane);
  return cloud;
}

function addManyHighConfidenceFlatSeedCells(cloud: MutableCloud, count = 80): void {
  // More than MAX_SEEDS=72 high-confidence flat seed cells. They live in coarse
  // regions away from the real corner and must not starve that corner merely
  // because their confidence is higher. Two points per 13-cm seed cell make each
  // eligible for seeding while remaining far too sparse/flat for corner evidence.
  for (let index = 0; index < count; index += 1) {
    const x = 2.0 + (index % 10) * 0.16;
    const y = Math.floor(index / 10) * 0.16;
    add(cloud, [x, y, 0], [0, 0, 1], 0.99);
    add(cloud, [x + 0.018, y + 0.018, 0], [0, 0, 1], 0.99);
  }
}

function nearestTo(
  corners: ReturnType<typeof detectCertifiedStructuralCorners>,
  target: readonly [number, number, number],
) {
  return [...corners].sort((left, right) =>
    Math.hypot(
      left.position[0] - target[0],
      left.position[1] - target[1],
      left.position[2] - target[2],
    ) - Math.hypot(
      right.position[0] - target[0],
      right.position[1] - target[1],
      right.position[2] - target[2],
    )
  )[0];
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
    const nearest = nearestTo(corners, [0, 0, 0]);
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

  it('is invariant to equivalent reversed surfel insertion order', () => {
    const source = cornerCloud(true);
    const forward = detectCertifiedStructuralCorners(snapshot(source));
    const reversed = detectCertifiedStructuralCorners(reversedSnapshot(source));
    expect(forward.length).toBeGreaterThan(0);
    expect(reversed.length).toBe(forward.length);
    const first = nearestTo(forward, [0, 0, 0]);
    const second = nearestTo(reversed, [0, 0, 0]);
    expect(second.position[0]).toBeCloseTo(first.position[0], 6);
    expect(second.position[1]).toBeCloseTo(first.position[1], 6);
    expect(second.position[2]).toBeCloseTo(first.position[2], 6);
    expect(second.confidence).toBeCloseTo(first.confidence, 6);
  });

  it('does not let more than 72 stronger flat seed cells starve a real corner in another region', () => {
    const source: MutableCloud = { positions: [], normals: [], confidence: [] };
    addManyHighConfidenceFlatSeedCells(source, 80);
    const target = [5.0, 0, 0] as const;
    appendCorner(source, target, true, 0.93);

    const corners = detectCertifiedStructuralCorners(snapshot(source));
    const nearest = nearestTo(corners, target);
    expect(nearest).toBeDefined();
    expect(Math.hypot(
      nearest.position[0] - target[0],
      nearest.position[1] - target[1],
      nearest.position[2] - target[2],
    )).toBeLessThan(0.04);
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

  it('withdraws a mature corner immediately when the current solve no longer has three-plane support', () => {
    const tracker = new CertifiedStructuralCornerTracker();
    const complete = snapshot(cornerCloud(true));
    tracker.update(complete, viewpoint(10, [0.5, 0.5, 0.5]));
    expect(tracker.update(complete, viewpoint(14, [0.28, 0.50, 0.55])).length).toBeGreaterThan(0);

    const missingThirdPlane = snapshot(cornerCloud(false));
    expect(tracker.update(missingThirdPlane, viewpoint(18, [0.15, 0.45, 0.55]))).toHaveLength(0);
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