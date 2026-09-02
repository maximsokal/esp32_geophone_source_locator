import { describe, expect, it } from 'vitest';

import { BoundedSpatialPointIndex } from '../../src/xr/boundedSpatialPointIndex';

describe('bounded spatial point index', () => {
  it('returns only exact-radius neighbours across voxel boundaries', () => {
    // Binary-exact fractions keep equal-distance ordering deterministic in Float32.
    const positions = Float32Array.from([
      0.0000, 0, 0,
      0.0625, 0, 0,
      0.1250, 0, 0,
      0.1875, 0, 0,
      0.3125, 0, 0,
    ]);
    const index = new BoundedSpatialPointIndex(positions, 5, 0.09);
    expect(index.queryRadius([0.09375, 0, 0], 0.105)).toEqual([1, 2, 0, 3]);
  });

  it('sorts equal-distance results by stable source index', () => {
    const positions = Float32Array.from([
      -0.0625, 0, 0,
      0.0625, 0, 0,
      0, -0.0625, 0,
      0, 0.0625, 0,
    ]);
    const index = new BoundedSpatialPointIndex(positions, 4, 0.06);
    expect(index.queryRadius([0, 0, 0], 0.07)).toEqual([0, 1, 2, 3]);
  });

  it('ignores non-finite points and never returns points outside the true sphere', () => {
    const positions = Float32Array.from([
      0, 0, 0,
      Number.NaN, 0, 0,
      0.09, 0.09, 0,
      0.20, 0, 0,
    ]);
    const index = new BoundedSpatialPointIndex(positions, 4, 0.10);
    expect(index.queryRadius([0, 0, 0], 0.10)).toEqual([0]);
  });
});
