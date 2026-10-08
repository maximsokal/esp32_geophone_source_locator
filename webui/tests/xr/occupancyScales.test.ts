import { describe, expect, it } from 'vitest';

import {
  fineOccupancyVoxelSizeMeters,
  unknownOccupancyVoxelSizeMeters,
} from '../../src/xr/occupancyScales';

describe('occupancy voxel scales', () => {
  it('uses fine spacing for FREE/OCCUPIED while retaining coarse UNKNOWN spacing', () => {
    const snapshot = {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
    };

    expect(fineOccupancyVoxelSizeMeters(snapshot)).toBe(0.1);
    expect(unknownOccupancyVoxelSizeMeters(snapshot)).toBe(0.4);
  });

  it('keeps legacy snapshots readable by falling back to their single occupancy spacing', () => {
    expect(fineOccupancyVoxelSizeMeters({ occupancyVoxelSizeMeters: 0.4 })).toBe(0.4);
    expect(unknownOccupancyVoxelSizeMeters({ occupancyVoxelSizeMeters: 0.4 })).toBe(0.4);
  });

  it('treats a null fine spacing as absent', () => {
    expect(fineOccupancyVoxelSizeMeters({
      occupancyFineVoxelSizeMeters: null,
      occupancyVoxelSizeMeters: 0.4,
    })).toBe(0.4);
  });
});
