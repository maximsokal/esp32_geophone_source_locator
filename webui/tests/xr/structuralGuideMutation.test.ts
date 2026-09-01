import { describe, expect, it } from 'vitest';

import {
  StructuralGuideTracker,
  detectStructuralGuideGeometry,
  type StructuralGuideInput,
} from '../../src/xr/structuralGuide';

function orthogonalCloud(planes: 2 | 3) {
  const positions: number[] = [];
  const normals: number[] = [];
  const confidence: number[] = [];
  const push = (x: number, y: number, z: number, nx: number, ny: number, nz: number) => {
    positions.push(x, y, z);
    normals.push(nx, ny, nz);
    confidence.push(0.95);
  };
  for (let a = -0.21; a <= 0.2101; a += 0.035) {
    for (let b = 0; b <= 0.4201; b += 0.035) {
      push(0, a, b, 1, 0, 0);
      push(a, 0, b, 0, 1, 0);
      if (planes === 3 && b <= 0.2101) push(a, b, 0, 0, 0, 1);
    }
  }
  return { positions, normals, confidence };
}

function input(
  cloud: ReturnType<typeof orthogonalCloud>,
  keyframeCount: number,
  camera: readonly [number, number, number],
  freeSpacePositions?: Float32Array,
): StructuralGuideInput {
  return {
    positions: Float32Array.from(cloud.positions),
    normals: Float32Array.from(cloud.normals),
    confidence: Float32Array.from(cloud.confidence),
    keyframeCount,
    voxelSizeMeters: 0.04,
    viewpoint: { position: camera, keyframeId: keyframeCount },
    freeSpacePositions,
    freeSpaceVoxelSizeMeters: freeSpacePositions === undefined ? undefined : 0.18,
  };
}

describe('structural guide mutation guards', () => {
  it('kills the mixed-normal-cell mutant: two real planes must never become a synthetic third plane', () => {
    const result = detectStructuralGuideGeometry(input(orthogonalCloud(2), 10, [0.5, 0.5, 0.5]));
    expect(result.edges.length).toBeGreaterThan(0);
    expect(result.corners).toHaveLength(0);
  });

  it('kills the global-keyframe mutant: temporal count alone cannot mature a candidate', () => {
    const cloud = orthogonalCloud(3);
    const tracker = new StructuralGuideTracker();
    for (const keyframe of [10, 20, 40, 80]) {
      const result = tracker.update(input(cloud, keyframe, [0.5, 0.5, 0.5]));
      expect(result.corners).toHaveLength(0);
    }
  });

  it('kills the occupancy-bypass mutant: verified free air cannot host an auto corner', () => {
    const cloud = orthogonalCloud(3);
    const baseline = detectStructuralGuideGeometry(input(cloud, 10, [0.5, 0.5, 0.5]));
    const corner = baseline.corners.reduce((best, candidate) =>
      Math.hypot(...candidate.position) < Math.hypot(...best.position) ? candidate : best,
    );
    const contradicted = detectStructuralGuideGeometry(
      input(cloud, 10, [0.5, 0.5, 0.5], Float32Array.from(corner.position)),
    );
    expect(contradicted.rejectedByFreeSpace).toBeGreaterThan(0);
    expect(contradicted.corners.some((candidate) => Math.hypot(...candidate.position) < 0.08)).toBe(false);
  });
});
