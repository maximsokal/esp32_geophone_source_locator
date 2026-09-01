import { describe, expect, it } from 'vitest';

import { clusterStructuralSurfaceFamilies } from '../../src/xr/structuralSurfaceClustering';

function snapshot(
  positions: readonly (readonly [number, number, number])[],
  normals: readonly (readonly [number, number, number])[],
) {
  return {
    positions: Float32Array.from(positions.flat()),
    normals: Float32Array.from(normals.flat()),
    confidence: new Float32Array(positions.length).fill(0.92),
    pointCount: positions.length,
    keyframeCount: 8,
    voxelSizeMeters: 0.04,
  };
}

describe('three-family structural surface clustering', () => {
  it('keeps three orthogonal surface families in the same fine structural cell', () => {
    const result = clusterStructuralSurfaceFamilies(snapshot(
      [
        [0.010, 0.010, 0.010],
        [0.012, 0.010, 0.011],
        [0.011, 0.013, 0.010],
      ],
      [
        [1, 0, 0],
        [0, 1, 0],
        [0, 0, 1],
      ],
    ));
    expect(result.pointCount).toBe(3);
    const dominantAxes = Array.from({ length: result.pointCount }, (_, index) => {
      const offset = index * 3;
      const values = [
        Math.abs(result.normals[offset]),
        Math.abs(result.normals[offset + 1]),
        Math.abs(result.normals[offset + 2]),
      ];
      return values.indexOf(Math.max(...values));
    }).sort();
    expect(dominantAxes).toEqual([0, 1, 2]);
  });

  it('merges opposite normal signs into one physical surface family', () => {
    const result = clusterStructuralSurfaceFamilies(snapshot(
      [
        [0.010, 0.010, 0.010],
        [0.012, 0.011, 0.010],
      ],
      [
        [1, 0, 0],
        [-1, 0, 0],
      ],
    ));
    expect(result.pointCount).toBe(1);
    expect(Math.abs(result.normals[0])).toBeGreaterThan(0.98);
  });

  it('does not admit a fourth conflicting manifold into one tiny cell', () => {
    const result = clusterStructuralSurfaceFamilies(snapshot(
      [
        [0.010, 0.010, 0.010],
        [0.012, 0.010, 0.011],
        [0.011, 0.013, 0.010],
        [0.013, 0.012, 0.012],
      ],
      [
        [1, 0, 0],
        [0, 1, 0],
        [0, 0, 1],
        [0.58, 0.58, 0.58],
      ],
    ));
    expect(result.pointCount).toBe(3);
  });

  it('keeps different spatial cells independent even when their normals match', () => {
    const result = clusterStructuralSurfaceFamilies(snapshot(
      [
        [0.010, 0.010, 0.010],
        [0.090, 0.010, 0.010],
      ],
      [
        [1, 0, 0],
        [1, 0, 0],
      ],
    ));
    expect(result.pointCount).toBe(2);
  });
});
