import { describe, expect, it } from 'vitest';

import { MAX_SURFELS_PER_FINE_VOXEL } from '../../src/xr/depthPointCloudV2';
import { clusterStructuralSurfaceFamilies } from '../../src/xr/structuralSurfaceClustering';

describe('fine-voxel structural orientation policy', () => {
  it('retains capacity for all three surfaces of a trihedral corner before downstream clustering', () => {
    expect(MAX_SURFELS_PER_FINE_VOXEL).toBe(3);
  });

  it('matches downstream structural clustering which can preserve three orthogonal families', () => {
    const result = clusterStructuralSurfaceFamilies({
      positions: Float32Array.from([
        0.010, 0.010, 0.010,
        0.012, 0.010, 0.011,
        0.011, 0.013, 0.010,
      ]),
      normals: Float32Array.from([
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
      ]),
      confidence: new Float32Array([0.92, 0.92, 0.92]),
      pointCount: 3,
      keyframeCount: 8,
      voxelSizeMeters: 0.04,
    });
    expect(result.pointCount).toBe(3);
  });
});
