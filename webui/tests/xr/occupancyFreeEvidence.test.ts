import { describe, expect, it } from 'vitest';

import {
  SparseOccupancyGrid,
  type OccupancyDepthFrameLike,
  type OccupancyFrameContext,
  type OccupancyViewLike,
} from '../../src/xr/occupancyEvidence';

const projection = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);
const view: OccupancyViewLike = {
  projectionMatrix: projection,
  transform: { matrix: new Float32Array(projection) },
};

function flatDepth(depthMeters = 1.5): OccupancyDepthFrameLike {
  return {
    width: 160,
    height: 90,
    getDepthInMeters: () => depthMeters,
  };
}

function context(keyframeId: number): OccupancyFrameContext {
  return {
    keyframeId,
    evidenceKeyframe: true,
    frameWeight: 1,
    cameraProjectPosition: [0, 0, 0],
    maxDepthMeters: 5,
  };
}

function buildFreeEvidence(): SparseOccupancyGrid {
  const grid = new SparseOccupancyGrid();
  for (let keyframeId = 1; keyframeId <= 6; keyframeId += 1) {
    grid.integrateDepthFrame(flatDepth(), view, context(keyframeId));
  }
  return grid;
}

describe('solver-specific physical FREE evidence', () => {
  it('returns every classified FREE voxel in an unbounded solver snapshot', () => {
    const grid = buildFreeEvidence();
    const display = grid.snapshot(null);
    const solver = grid.freeSpaceEvidenceSnapshot();

    expect(solver.knownFreeVoxelCount).toBe(display.knownFreeVoxelCount);
    expect(solver.includedFreeVoxelCount).toBe(solver.knownFreeVoxelCount);
    expect(solver.freeAirPositions.length / 3).toBe(solver.knownFreeVoxelCount);
    expect(solver.knownFreeVoxelCount).toBeGreaterThan(20);

    for (let index = 0; index < solver.freeAirPositions.length; index += 3) {
      expect(grid.classifyPoint([
        solver.freeAirPositions[index],
        solver.freeAirPositions[index + 1],
        solver.freeAirPositions[index + 2],
      ])).toBe('free');
    }
  });

  it('spatially filters solver evidence without changing FREE classification or exact global count', () => {
    const grid = buildFreeEvidence();
    const all = grid.freeSpaceEvidenceSnapshot();
    const bounded = grid.freeSpaceEvidenceSnapshot({
      min: [-2, 0.36, -2],
      max: [2, 0.90, 2],
    });

    expect(bounded.knownFreeVoxelCount).toBe(all.knownFreeVoxelCount);
    expect(bounded.includedFreeVoxelCount).toBeGreaterThan(0);
    expect(bounded.includedFreeVoxelCount).toBeLessThan(all.includedFreeVoxelCount);
    expect(bounded.freeAirPositions.length / 3).toBe(bounded.includedFreeVoxelCount);

    for (let index = 0; index < bounded.freeAirPositions.length; index += 3) {
      const point = [
        bounded.freeAirPositions[index],
        bounded.freeAirPositions[index + 1],
        bounded.freeAirPositions[index + 2],
      ] as const;
      expect(point[1]).toBeGreaterThanOrEqual(0.36);
      expect(point[1]).toBeLessThanOrEqual(0.90);
      expect(grid.classifyPoint(point)).toBe('free');
    }
  });

  it('applies the relevance margin without turning UNKNOWN or OCCUPIED cells into FREE evidence', () => {
    const grid = buildFreeEvidence();
    const strict = grid.freeSpaceEvidenceSnapshot({ min: [-0.05, 0.95, -0.05], max: [0.05, 1.05, 0.05] });
    const expanded = grid.freeSpaceEvidenceSnapshot(
      { min: [-0.05, 0.95, -0.05], max: [0.05, 1.05, 0.05] },
      0.30,
    );

    expect(expanded.includedFreeVoxelCount).toBeGreaterThanOrEqual(strict.includedFreeVoxelCount);
    for (let index = 0; index < expanded.freeAirPositions.length; index += 3) {
      expect(grid.classifyPoint([
        expanded.freeAirPositions[index],
        expanded.freeAirPositions[index + 1],
        expanded.freeAirPositions[index + 2],
      ])).toBe('free');
    }
  });

  it('clears classified FREE indices on reset', () => {
    const grid = buildFreeEvidence();
    expect(grid.freeSpaceEvidenceSnapshot().knownFreeVoxelCount).toBeGreaterThan(0);
    grid.reset();
    const reset = grid.freeSpaceEvidenceSnapshot();
    expect(reset.knownFreeVoxelCount).toBe(0);
    expect(reset.includedFreeVoxelCount).toBe(0);
    expect(reset.freeAirPositions).toHaveLength(0);
  });
});
