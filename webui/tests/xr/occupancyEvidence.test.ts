import { describe, expect, it } from 'vitest';

import {
  SparseOccupancyGrid,
  occupancyMathForTests,
  type OccupancyDepthFrameLike,
  type OccupancyFrameContext,
  type OccupancyViewLike,
} from '../../src/xr/occupancyEvidence';
import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloud';

const projection = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);
const identity = new Float32Array(projection);
const view: OccupancyViewLike = {
  projectionMatrix: projection,
  transform: { matrix: identity },
};

function flatDepth(depthMeters = 1.5): OccupancyDepthFrameLike {
  return {
    width: 160,
    height: 90,
    getDepthInMeters: () => depthMeters,
  };
}

function context(
  keyframeId: number,
  evidenceKeyframe = true,
  cameraProjectPosition: readonly [number, number, number] = [0, 0, 0],
  frameWeight = 1,
): OccupancyFrameContext {
  return {
    keyframeId,
    evidenceKeyframe,
    frameWeight,
    cameraProjectPosition,
    maxDepthMeters: 5,
  };
}

const roomBounds = {
  min: [-1.8, 0, -1.8] as const,
  max: [1.8, 2.2, 1.8] as const,
};

function translatedView(x: number): OccupancyViewLike {
  return {
    projectionMatrix: projection,
    transform: {
      matrix: new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        x, 0, 0, 1,
      ]),
    },
  };
}

describe('occupancy log-odds and ray traversal', () => {
  it('round-trips probabilities through log odds', () => {
    for (const probability of [0.1, 0.3, 0.5, 0.7, 0.95]) {
      const logOdds = occupancyMathForTests.probabilityToLogOdds(probability);
      expect(occupancyMathForTests.logOddsToProbability(logOdds)).toBeCloseTo(probability, 10);
    }
  });

  it('traces a 3D segment without duplicate voxels and includes both ends', () => {
    const keys = occupancyMathForTests.traceSegmentVoxelKeys(
      [0.01, 0.01, 0.01],
      [0.91, 0.73, 0.55],
      0.18,
    );
    expect(keys[0]).toBe('0:0:0');
    expect(keys.at(-1)).toBe('5:4:3');
    expect(new Set(keys).size).toBe(keys.length);
    expect(keys.length).toBeGreaterThan(5);
  });

  it('handles a negative-direction ray deterministically', () => {
    const keys = occupancyMathForTests.traceSegmentVoxelKeys(
      [0.41, 0.41, 0.41],
      [-0.41, -0.23, -0.05],
      0.18,
    );
    expect(keys[0]).toBe('2:2:2');
    expect(keys.at(-1)).toBe('-3:-2:-1');
    expect(new Set(keys).size).toBe(keys.length);
  });
});

describe('OctoMap-style free / occupied / unknown evidence', () => {
  it('does not let polish frames alter occupancy', () => {
    const grid = new SparseOccupancyGrid();
    const result = grid.integrateDepthFrame(flatDepth(), view, context(1, false));
    expect(result.integrated).toBe(false);
    expect(grid.cellCount).toBe(0);
  });

  it('updates one evidence keyframe only once even if called repeatedly', () => {
    const grid = new SparseOccupancyGrid();
    const first = grid.integrateDepthFrame(flatDepth(), view, context(7));
    const cellsAfterFirst = grid.cellCount;
    const probabilityAfterFirst = grid.probabilityAt([0, 1.5, 0]);

    for (let repeat = 0; repeat < 20; repeat += 1) {
      const repeated = grid.integrateDepthFrame(flatDepth(), view, context(7));
      expect(repeated.integrated).toBe(false);
    }

    expect(first.integrated).toBe(true);
    expect(grid.cellCount).toBe(cellsAfterFirst);
    expect(grid.probabilityAt([0, 1.5, 0])).toBe(probabilityAfterFirst);
  });

  it('keeps a single-view observation uncertain instead of declaring space final', () => {
    const grid = new SparseOccupancyGrid();
    grid.integrateDepthFrame(flatDepth(), view, context(1));
    const snapshot = grid.snapshot(roomBounds);
    expect(snapshot.knownFreeVoxelCount).toBe(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBe(0);
    expect(snapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
  });

  it('needs repeated independent keyframes before carving known free air', () => {
    const grid = new SparseOccupancyGrid();
    grid.integrateDepthFrame(flatDepth(), view, context(1));
    grid.integrateDepthFrame(flatDepth(), view, context(2));
    expect(grid.snapshot(roomBounds).knownFreeVoxelCount).toBe(0);

    grid.integrateDepthFrame(flatDepth(), view, context(3));
    const snapshot = grid.snapshot(roomBounds);
    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(20);
  });

  it('promotes repeatedly measured depth endpoints to occupied surface evidence', () => {
    const grid = new SparseOccupancyGrid();
    grid.integrateDepthFrame(flatDepth(), view, context(1));
    grid.integrateDepthFrame(flatDepth(), view, context(2));
    grid.integrateDepthFrame(flatDepth(), view, context(3));
    const snapshot = grid.snapshot(roomBounds);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(10);
    expect(snapshot.occupiedSurfacePositions.length).toBeGreaterThan(0);
  });

  it('never ray-carves the unseen volume behind the measured surface', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframeId = 1; keyframeId <= 8; keyframeId += 1) {
      grid.integrateDepthFrame(flatDepth(), view, context(keyframeId));
    }
    expect(grid.classifyPoint([0, 1.95, 0])).toBe('unknown');
    expect(grid.probabilityAt([0, 1.95, 0])).toBe(0.5);
  });

  it('keeps known-free display voxels in front of the measured boundary', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframeId = 1; keyframeId <= 6; keyframeId += 1) {
      grid.integrateDepthFrame(flatDepth(), view, context(keyframeId));
    }
    const snapshot = grid.snapshot(roomBounds);
    let maximumFreeDepth = Number.NEGATIVE_INFINITY;
    for (let index = 1; index < snapshot.freeAirPositions.length; index += 3) {
      maximumFreeDepth = Math.max(maximumFreeDepth, snapshot.freeAirPositions[index]);
    }
    expect(maximumFreeDepth).toBeLessThan(1.5);
  });

  it('never labels the same display voxel both free and occupied', () => {
    const grid = new SparseOccupancyGrid();
    for (let keyframeId = 1; keyframeId <= 6; keyframeId += 1) {
      grid.integrateDepthFrame(flatDepth(), view, context(keyframeId));
    }
    const snapshot = grid.snapshot(roomBounds);
    const keys = (positions: Float32Array) => {
      const output = new Set<string>();
      for (let index = 0; index < positions.length; index += 3) {
        output.add(`${positions[index].toFixed(3)}:${positions[index + 1].toFixed(3)}:${positions[index + 2].toFixed(3)}`);
      }
      return output;
    };
    const free = keys(snapshot.freeAirPositions);
    const occupied = keys(snapshot.occupiedSurfacePositions);
    for (const key of occupied) expect(free.has(key)).toBe(false);
  });

  it('rejects depth beyond the configured rescue-scene range', () => {
    const grid = new SparseOccupancyGrid();
    const result = grid.integrateDepthFrame(flatDepth(6.0), view, context(1));
    expect(result.integrated).toBe(true);
    expect(result.sampledRays).toBe(0);
    expect(grid.cellCount).toBe(0);
  });

  it('keeps sparse occupancy memory within its hard cell budget', () => {
    const grid = new SparseOccupancyGrid(0.08, 1000);
    for (let keyframeId = 1; keyframeId <= 30; keyframeId += 1) {
      const translation = keyframeId * 0.23;
      grid.integrateDepthFrame(
        flatDepth(),
        translatedView(translation),
        context(keyframeId, true, [translation, 0, 0]),
      );
      expect(grid.cellCount).toBeLessThanOrEqual(1000);
    }
  });
});

describe('surface reconstruction + occupancy integration', () => {
  it('returns surface, known-air and unknown-candidate layers from one scan snapshot', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const depth = flatDepth();
    const cameras: Array<readonly [number, number, number]> = [
      [0, 0, 0],
      [0.45, 0, 0],
      [-0.45, 0, 0],
      [0.32, 0.20, 0],
    ];

    cameras.forEach((camera, index) => {
      accumulator.sampleDepthFrame(
        depth,
        view,
        {
          keyframeId: index + 1,
          evidenceKeyframe: true,
          frameWeight: 1,
          cameraProjectPosition: camera,
        },
      );
    });

    const snapshot = accumulator.snapshot(cameras.length, cameras.length);
    expect(snapshot.pointCount).toBeGreaterThan(0);
    expect(snapshot.occupancyVoxelSizeMeters).toBeGreaterThan(snapshot.voxelSizeMeters);
    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(0);
    expect(snapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
  });
});
