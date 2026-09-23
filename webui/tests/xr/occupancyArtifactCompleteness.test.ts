import { describe, expect, it } from 'vitest';

import { SparseOccupancyGrid } from '../../src/xr/occupancyEvidence';

/*
 * MAX-213: the saved scan artifact is built from SparseOccupancyGrid.snapshot().
 * It used to cap FREE/OCCUPIED/UNKNOWN at 12000 points each while reporting the
 * full counts, report a stride-scaled voxel size for fine-cell centers, and
 * silently stop creating cells at maxCells.
 */

type UpdateCell = (key: string, occupied: boolean, weight: number, keyframeId: number) => void;

function updater(grid: SparseOccupancyGrid): UpdateCell {
  const internal = grid as unknown as { updateCell: UpdateCell };
  return internal.updateCell.bind(grid);
}

function markFree(grid: SparseOccupancyGrid, keys: string[]): void {
  const update = updater(grid);
  for (let keyframe = 1; keyframe <= 3; keyframe += 1) {
    for (const key of keys) update(key, false, 1, keyframe);
  }
}

describe('occupancy artifact completeness (MAX-213)', () => {
  it('keeps every FREE cell in the saved arrays beyond the old 12000 display cap', () => {
    const grid = new SparseOccupancyGrid(0.18, 50000);
    const keys: string[] = [];
    for (let x = 0; x < 30; x += 1) {
      for (let y = 0; y < 30; y += 1) {
        for (let z = 0; z < 15; z += 1) keys.push(`${x}:${y}:${z}`);
      }
    }
    markFree(grid, keys);

    const snapshot = grid.snapshot(null);
    expect(snapshot.knownFreeVoxelCount).toBe(13500);
    expect(snapshot.freeAirPositions.length / 3).toBe(snapshot.knownFreeVoxelCount);
    expect(snapshot.occupancyCellsSaturated).toBe(false);
  });

  it('reports strided UNKNOWN points at the centers of the declared coarse cells', () => {
    const grid = new SparseOccupancyGrid(0.1, 50000);
    // 8 m x 8 m x 4 m at 0.1 m = 2.56 M fine cells -> stride > 1.
    const snapshot = grid.snapshot({ min: [-4, -4, -2], max: [4, 4, 2] });
    const size = snapshot.occupancyVoxelSizeMeters;
    expect(size).toBeGreaterThan(0.1);
    expect(snapshot.unknownCandidatePositions.length / 3).toBe(snapshot.unknownCandidateVoxelCount);

    const seen = new Set<string>();
    for (let index = 0; index < snapshot.unknownCandidatePositions.length; index += 3) {
      const cell = [0, 1, 2].map((axis) => Math.floor(snapshot.unknownCandidatePositions[index + axis] / size));
      // Each point sits at the center of its own declared coarse cell ...
      for (let axis = 0; axis < 3; axis += 1) {
        expect(snapshot.unknownCandidatePositions[index + axis]).toBeCloseTo((cell[axis] + 0.5) * size, 5);
      }
      // ... and no two points collapse into the same cell when re-hashed.
      const key = cell.join(':');
      expect(seen.has(key)).toBe(false);
      seen.add(key);
    }
  });

  it('flags a saturated cell budget instead of silently leaving areas UNKNOWN', () => {
    const grid = new SparseOccupancyGrid(0.18, 1000);
    const keys: string[] = [];
    for (let index = 0; index < 1100; index += 1) keys.push(`${index}:0:0`);
    markFree(grid, keys);

    const snapshot = grid.snapshot(null);
    expect(grid.cellCount).toBe(1000);
    expect(snapshot.occupancyCellsSaturated).toBe(true);
    expect(snapshot.occupancyDroppedCellUpdates).toBeGreaterThan(0);

    grid.reset();
    expect(grid.snapshot(null).occupancyCellsSaturated).toBe(false);
  });
});
