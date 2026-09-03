import { describe, expect, it } from 'vitest';

import {
  CertifiedStructuralCornerTracker,
  detectCertifiedStructuralCorners,
} from '../../src/xr/certifiedStructuralGeometry';
import type { ManualPlacementSurfaceSnapshot } from '../../src/xr/manualPlacementSurface';
import { rejectStructuralCandidatesInVerifiedFreeSpace } from '../../src/xr/structuralFreeSpaceGate';
import type { ProjectPoint, StructuralGuideViewpoint } from '../../src/xr/structuralGuide';

interface StressCloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function stressScene(iteration = 0): StressCloud {
  const cloud: StressCloud = { positions: [], normals: [], confidence: [] };
  const jitter = (axis: number, a: number, b: number): number =>
    0.0018 * Math.sin(iteration * 0.31 + axis * 1.7 + a * 17 + b * 23);
  const push = (
    x: number,
    y: number,
    z: number,
    nx: number,
    ny: number,
    nz: number,
    confidence = 0.94,
  ): void => {
    cloud.positions.push(x, y, z);
    cloud.normals.push(nx, ny, nz);
    cloud.confidence.push(confidence);
  };

  for (let a = -0.42; a <= 0.4201; a += 0.035) {
    for (let b = 0; b <= 0.7001; b += 0.035) {
      push(jitter(0, a, b), a, b, 1, 0, 0);
      push(a, jitter(1, a, b), b, 0, 1, 0);
      if (b <= 0.4201) push(a, b, jitter(2, a, b), 0, 0, 1);
    }
  }

  // Keep the browser stress fixture's low-confidence clutter exactly represented.
  for (let index = 0; index < 180; index += 1) {
    const x = -0.45 + (index % 18) * 0.05;
    const y = -0.40 + (Math.floor(index / 18) % 10) * 0.08;
    const z = 0.10 + 0.16 * Math.sin(index * 0.71);
    push(x, y, z, 0.577, 0.577, 0.577, 0.28);
  }

  return cloud;
}

function snapshot(
  cloud: StressCloud,
  freeSpacePositions?: Float32Array,
): ManualPlacementSurfaceSnapshot {
  return {
    positions: Float32Array.from(cloud.positions),
    normals: Float32Array.from(cloud.normals),
    confidence: Float32Array.from(cloud.confidence),
    pointCount: cloud.confidence.length,
    voxelSizeMeters: 0.04,
    freeSpacePositions,
    freeSpaceVoxelSizeMeters: freeSpacePositions === undefined ? undefined : 0.18,
  };
}

function viewpoint(
  keyframeId: number,
  position: ProjectPoint,
): StructuralGuideViewpoint {
  return { keyframeId, position };
}

function occupancyVoxelCenter(point: ProjectPoint, size = 0.18): ProjectPoint {
  return [
    (Math.floor(point[0] / size) + 0.5) * size,
    (Math.floor(point[1] / size) + 0.5) * size,
    (Math.floor(point[2] / size) + 0.5) * size,
  ];
}

function occupancyVoxelKey(point: ProjectPoint, size = 0.18): string {
  return `${Math.floor(point[0] / size)}:${Math.floor(point[1] / size)}:${Math.floor(point[2] / size)}`;
}

describe('production structural corner pipeline on the Chrome stress scene', () => {
  it('certifies the same noisy jittered three-plane scene used by structural Chrome stress', () => {
    const corners = detectCertifiedStructuralCorners(snapshot(stressScene(0)));
    expect(corners.length).toBeGreaterThan(0);
    const nearest = [...corners].sort(
      (left, right) => Math.hypot(...left.position) - Math.hypot(...right.position),
    )[0];
    expect(Math.hypot(...nearest.position)).toBeLessThan(0.08);
  });

  it('matures the jittered certified corner only after an independent viewpoint', () => {
    const source = snapshot(stressScene(0));
    const tracker = new CertifiedStructuralCornerTracker();
    for (const keyframeId of [10, 20, 40, 80]) {
      expect(tracker.update(source, viewpoint(keyframeId, [0.55, 0.55, 0.55]))).toHaveLength(0);
    }
    const matured = tracker.update(source, viewpoint(84, [0.28, 0.52, 0.60]));
    expect(matured.length).toBeGreaterThan(0);
    expect(matured[0].observations).toBeGreaterThanOrEqual(2);
  });

  it('vetoes the certified corner only in its exact ray-verified FREE voxel', () => {
    const source = stressScene(0);
    const baseline = detectCertifiedStructuralCorners(snapshot(source));
    expect(baseline.length).toBeGreaterThan(0);
    const nearest = [...baseline].sort(
      (left, right) => Math.hypot(...left.position) - Math.hypot(...right.position),
    )[0];
    const freePosition = occupancyVoxelCenter(nearest.position);
    const freeVoxelKey = occupancyVoxelKey(freePosition);
    const gated = rejectStructuralCandidatesInVerifiedFreeSpace(
      baseline,
      snapshot(source, Float32Array.from(freePosition)),
    );

    expect(gated.rejectedByFreeSpace).toBeGreaterThan(0);
    expect(
      gated.candidates.some((corner) => occupancyVoxelKey(corner.position) === freeVoxelKey),
    ).toBe(false);
  });
});
