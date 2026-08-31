import { describe, expect, it } from 'vitest';

import {
  SparseOccupancyGrid,
  type OccupancyDepthFrameLike,
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
  transform: { matrix: projection },
};
const depth: OccupancyDepthFrameLike = {
  width: 160,
  height: 90,
  getDepthInMeters: () => 1.5,
};

function integrate(grid: SparseOccupancyGrid, keyframeId: number, frameWeight = 1): void {
  grid.integrateDepthFrame(depth, view, {
    keyframeId,
    evidenceKeyframe: true,
    frameWeight,
    cameraProjectPosition: [0, 0, 0],
    maxDepthMeters: 5,
  });
}

describe('rescue search-volume semantics', () => {
  it('represents a completely unobserved envelope as unknown without allocating evidence cells', () => {
    const grid = new SparseOccupancyGrid();
    const snapshot = grid.snapshot({ min: [-0.5, 1.2, -0.5], max: [0.5, 1.8, 0.5] });
    expect(snapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
    expect(snapshot.knownFreeVoxelCount).toBe(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBe(0);
    expect(grid.cellCount).toBe(0);
  });

  it('reports ray-verified exterior air even when it lies outside the candidate rubble envelope', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframe = 1; keyframe <= 6; keyframe += 1) integrate(grid, keyframe);
    const snapshot = grid.snapshot({ min: [-1.4, 1.38, -1.4], max: [1.4, 1.62, 1.4] });
    let exteriorFree = false;
    for (let index = 1; index < snapshot.freeAirPositions.length; index += 3) {
      if (snapshot.freeAirPositions[index] < 1.38) exteriorFree = true;
    }
    expect(exteriorFree).toBe(true);
    expect(snapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
  });

  it('keeps UNKNOWN display samples inside the measured object envelope up to voxel quantization', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframe = 1; keyframe <= 4; keyframe += 1) integrate(grid, keyframe);
    const bounds = { min: [-0.45, 1.35, -0.45] as const, max: [0.45, 1.65, 0.45] as const };
    const snapshot = grid.snapshot(bounds);
    const epsilon = snapshot.occupancyVoxelSizeMeters;
    for (let index = 0; index < snapshot.unknownCandidatePositions.length; index += 3) {
      const x = snapshot.unknownCandidatePositions[index];
      const y = snapshot.unknownCandidatePositions[index + 1];
      const z = snapshot.unknownCandidatePositions[index + 2];
      expect(x).toBeGreaterThanOrEqual(bounds.min[0] - epsilon);
      expect(x).toBeLessThanOrEqual(bounds.max[0] + epsilon);
      expect(y).toBeGreaterThanOrEqual(bounds.min[1] - epsilon);
      expect(y).toBeLessThanOrEqual(bounds.max[1] + epsilon);
      expect(z).toBeGreaterThanOrEqual(bounds.min[2] - epsilon);
      expect(z).toBeLessThanOrEqual(bounds.max[2] + epsilon);
    }
  });

  it('does not let zero-quality evidence allocate or classify occupancy', () => {
    const grid = new SparseOccupancyGrid();
    const result = grid.integrateDepthFrame(depth, view, {
      keyframeId: 1,
      evidenceKeyframe: true,
      frameWeight: 0,
      cameraProjectPosition: [0, 0, 0],
      maxDepthMeters: 5,
    });
    expect(result.integrated).toBe(false);
    expect(grid.cellCount).toBe(0);
  });

  it('keeps weak repeated ray evidence conservative instead of quickly saturating free space', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframe = 1; keyframe <= 6; keyframe += 1) integrate(grid, keyframe, 0.1);
    const snapshot = grid.snapshot({ min: [-1.5, 0, -1.5], max: [1.5, 1.8, 1.5] });
    expect(snapshot.knownFreeVoxelCount).toBe(0);
    expect(snapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
  });

  it('reset removes accumulated free and occupied evidence', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframe = 1; keyframe <= 5; keyframe += 1) integrate(grid, keyframe);
    expect(grid.cellCount).toBeGreaterThan(0);
    grid.reset();
    expect(grid.cellCount).toBe(0);
    expect(grid.classifyPoint([0, 0.4, 0])).toBe('unknown');
    expect(grid.probabilityAt([0, 0.4, 0])).toBe(0.5);
  });
});
