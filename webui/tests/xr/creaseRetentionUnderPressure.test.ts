import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import {
  ARCORE_NOISE, boxCornerScene, distanceToCrease, orbitPoses, polygonAreaSplit, scanScene,
} from './support/syntheticDepthScene';

/*
 * MAX-257 (regression, confirmed not-a-bug on 2026-09-24): under memory
 * pressure the retention policy must keep at least an area-proportional share
 * of points next to creases. Measured: without pressure 3.1x the area share
 * (creases are over-sampled), at the minimum budget (1000, ~67 % of the
 * unconstrained 1489 representatives, > 1000 evictions) 2.2x. The drop only
 * matters if coverage approaches area-proportional, so the bar is 1.5x.
 */

describe('crease retention under memory pressure', () => {
  it('keeps crease coverage well above area-proportional (>= 1.5x) under memory pressure', () => {
    const voxel = 0.04;
    const scene = boxCornerScene([0, 0, 0], 2.4);
    const poses = [...orbitPoses(scene.focus, 1.4, 35, 5, 85, 16), ...orbitPoses([-0.3, -0.3, -0.3], 1.0, 35, 5, 85, 16)];
    const split = polygonAreaSplit(scene, voxel);
    const areaShare = split.nearCrease / split.total;
    const creaseRatio = (budget: number): { ratio: number; evictions: number } => {
      const accumulator = new DepthPointCloudAccumulator(voxel, budget);
      scanScene(accumulator, scene, poses, { noise: ARCORE_NOISE });
      const s = accumulator.snapshot(poses.length, poses.length);
      let near = 0;
      for (let i = 0; i < s.pointCount; i += 1) {
        if (distanceToCrease(scene, [s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]]) < voxel) near += 1;
      }
      return { ratio: near / s.pointCount / areaShare, evictions: s.rebalanceEvictionCount + s.replacementCount };
    };
    const free = creaseRatio(1_000_000);
    // 1000 is the constructor's minimum budget: the tightest pressure available.
    const pressed = creaseRatio(1000);
    expect(pressed.evictions).toBeGreaterThan(free.evictions); // pressure really applied
    expect(free.ratio).toBeGreaterThanOrEqual(1.5);
    expect(pressed.ratio).toBeGreaterThanOrEqual(1.5);
  });
});
