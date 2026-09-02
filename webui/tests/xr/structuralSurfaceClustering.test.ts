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

function normal(angleDegrees: number): readonly [number, number, number] {
  const angle = angleDegrees * Math.PI / 180;
  return [Math.sin(angle), 0, Math.cos(angle)];
}

describe('feature-aware structural surface clustering', () => {
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

  it('preserves more representatives on a convex bend than on a flat patch', () => {
    const positions = [
      [0.004, 0.010, 0.010],
      [0.014, 0.010, 0.010],
      [0.024, 0.010, 0.010],
      [0.034, 0.010, 0.010],
      [0.044, 0.010, 0.010],
      [0.052, 0.010, 0.010],
    ] as const;
    const flat = clusterStructuralSurfaceFamilies(snapshot(
      positions,
      positions.map(() => normal(0)),
    ));
    const convex = clusterStructuralSurfaceFamilies(snapshot(
      positions,
      [-28, -18, -8, 8, 18, 28].map(normal),
    ));

    expect(flat.pointCount).toBe(1);
    expect(convex.pointCount).toBeGreaterThan(flat.pointCount);
    // Do not force arbitrary over-segmentation: two robust local pieces are
    // already more informative than flattening the whole bend into one normal.
    expect(convex.pointCount).toBeGreaterThanOrEqual(2);
  });

  it('does not lose a hard edge merely because it falls exactly across base-cell boundaries', () => {
    // baseCellSize is 0.054 m for the default 0.04 m voxel size. Each half below
    // is internally flat, so a per-cell-only classifier would see two flat cells
    // and fail to request feature detail. Neighbourhood classification must see
    // both supported normal families across the 0.054 m boundary.
    const positions = [
      [0.034, 0.010, 0.010],
      [0.044, 0.010, 0.010],
      [0.052, 0.010, 0.010],
      [0.056, 0.010, 0.010],
      [0.068, 0.010, 0.010],
      [0.082, 0.010, 0.010],
    ] as const;
    const flat = clusterStructuralSurfaceFamilies(snapshot(
      positions,
      positions.map(() => normal(0)),
    ));
    const acrossBoundaryEdge = clusterStructuralSurfaceFamilies(snapshot(
      positions,
      [normal(0), normal(0), normal(0), normal(62), normal(62), normal(62)],
    ));

    expect(flat.pointCount).toBe(2);
    expect(acrossBoundaryEdge.pointCount).toBeGreaterThan(flat.pointCount);
  });

  it('keeps a smooth bend crossing a base-cell boundary denser than the equivalent flat strip', () => {
    const positions = [
      [0.034, 0.010, 0.010],
      [0.044, 0.010, 0.010],
      [0.052, 0.010, 0.010],
      [0.056, 0.010, 0.010],
      [0.068, 0.010, 0.010],
      [0.082, 0.010, 0.010],
    ] as const;
    const flat = clusterStructuralSurfaceFamilies(snapshot(positions, positions.map(() => normal(0))));
    const convex = clusterStructuralSurfaceFamilies(snapshot(
      positions,
      [-28, -18, -8, 8, 18, 28].map(normal),
    ));
    expect(convex.pointCount).toBeGreaterThan(flat.pointCount);
  });

  it('does not over-segment a flat patch because of one abnormal normal', () => {
    const positions = [
      [0.004, 0.010, 0.010],
      [0.014, 0.010, 0.010],
      [0.024, 0.010, 0.010],
      [0.034, 0.010, 0.010],
      [0.044, 0.010, 0.010],
      [0.052, 0.010, 0.010],
    ] as const;
    const result = clusterStructuralSurfaceFamilies(snapshot(
      positions,
      [normal(-2), normal(-1), normal(0), normal(1), normal(2), normal(88)],
    ));
    expect(result.pointCount).toBeLessThanOrEqual(2);
  });
});
