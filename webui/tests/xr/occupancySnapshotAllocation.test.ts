import { describe, expect, it, vi } from 'vitest';

import {
  SparseOccupancyGrid,
  type OccupancyBounds,
  type OccupancyPoint,
  type OccupancySnapshot,
} from '../../src/xr/occupancyEvidence';

const MAX_ENUMERATED_VOLUME_CELLS = 120_000;

interface OccupancyGridInternals {
  voxelSizeMeters: number;
  cells: Map<string, unknown>;
  freeCellKeys: Set<string>;
  occupiedCellKeys: Set<string>;
  droppedCellUpdates: number;
  classifyCell(cell: unknown): 'free' | 'occupied' | 'unknown';
  updateCell(key: string, occupied: boolean, weight: number, keyframeId: number): void;
}

function internals(grid: SparseOccupancyGrid): OccupancyGridInternals {
  return grid as unknown as OccupancyGridInternals;
}

function markClassified(
  grid: SparseOccupancyGrid,
  keys: readonly string[],
  occupied: boolean,
): void {
  const updateCell = internals(grid).updateCell.bind(grid);
  for (let keyframeId = 1; keyframeId <= 3; keyframeId += 1) {
    for (const key of keys) updateCell(key, occupied, 1, keyframeId);
  }
}

function keyFromCoordinates(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function oldVoxelCenterFromKey(key: string, voxelSizeMeters: number): OccupancyPoint {
  const [xText, yText, zText] = key.split(':');
  return [
    (Number(xText) + 0.5) * voxelSizeMeters,
    (Number(yText) + 0.5) * voxelSizeMeters,
    (Number(zText) + 0.5) * voxelSizeMeters,
  ];
}

/*
 * MAX-404 parity oracle: this intentionally keeps the pre-change boxed-array
 * snapshot and UNKNOWN enumeration algorithm from occupancyEvidence.ts.
 */
function oldSnapshotOracle(
  grid: SparseOccupancyGrid,
  candidateBounds: OccupancyBounds | null,
): OccupancySnapshot {
  const state = internals(grid);
  const free: number[] = [];
  const occupied: number[] = [];

  for (const key of state.freeCellKeys) free.push(...oldVoxelCenterFromKey(key, state.voxelSizeMeters));
  for (const key of state.occupiedCellKeys) occupied.push(...oldVoxelCenterFromKey(key, state.voxelSizeMeters));

  let occupancyVoxelSizeMeters = state.voxelSizeMeters;
  const unknown: number[] = [];
  if (candidateBounds !== null) {
    const minX = Math.floor(candidateBounds.min[0] / state.voxelSizeMeters);
    const minY = Math.floor(candidateBounds.min[1] / state.voxelSizeMeters);
    const minZ = Math.floor(candidateBounds.min[2] / state.voxelSizeMeters);
    const maxX = Math.floor(candidateBounds.max[0] / state.voxelSizeMeters);
    const maxY = Math.floor(candidateBounds.max[1] / state.voxelSizeMeters);
    const maxZ = Math.floor(candidateBounds.max[2] / state.voxelSizeMeters);
    const nx = Math.max(1, maxX - minX + 1);
    const ny = Math.max(1, maxY - minY + 1);
    const nz = Math.max(1, maxZ - minZ + 1);
    const totalCells = nx * ny * nz;
    const stride = Math.max(1, Math.ceil(Math.cbrt(totalCells / MAX_ENUMERATED_VOLUME_CELLS)));
    const half = Math.floor(stride / 2);
    occupancyVoxelSizeMeters = state.voxelSizeMeters * stride;

    for (let cx = Math.floor(minX / stride); cx <= Math.floor(maxX / stride); cx += 1) {
      for (let cy = Math.floor(minY / stride); cy <= Math.floor(maxY / stride); cy += 1) {
        for (let cz = Math.floor(minZ / stride); cz <= Math.floor(maxZ / stride); cz += 1) {
          if (state.classifyCell(state.cells.get(keyFromCoordinates(
            cx * stride + half,
            cy * stride + half,
            cz * stride + half,
          ))) !== 'unknown') continue;
          unknown.push(
            (cx + 0.5) * occupancyVoxelSizeMeters,
            (cy + 0.5) * occupancyVoxelSizeMeters,
            (cz + 0.5) * occupancyVoxelSizeMeters,
          );
        }
      }
    }
  }

  return {
    occupancyVoxelSizeMeters,
    knownFreeVoxelCount: state.freeCellKeys.size,
    occupiedSurfaceVoxelCount: state.occupiedCellKeys.size,
    unknownCandidateVoxelCount: unknown.length / 3,
    freeAirPositions: Float32Array.from(free),
    occupiedSurfacePositions: Float32Array.from(occupied),
    unknownCandidatePositions: Float32Array.from(unknown),
    occupancyCellsSaturated: state.droppedCellUpdates > 0,
    occupancyFineVoxelSizeMeters: state.voxelSizeMeters,
    occupancyDroppedCellUpdates: state.droppedCellUpdates,
  };
}

function expectSnapshotMatchesOracle(
  grid: SparseOccupancyGrid,
  candidateBounds: OccupancyBounds | null,
): OccupancySnapshot {
  const expected = oldSnapshotOracle(grid, candidateBounds);
  const actual = grid.snapshot(candidateBounds);

  expect(actual.occupancyVoxelSizeMeters).toBe(expected.occupancyVoxelSizeMeters);
  expect(actual.knownFreeVoxelCount).toBe(expected.knownFreeVoxelCount);
  expect(actual.occupiedSurfaceVoxelCount).toBe(expected.occupiedSurfaceVoxelCount);
  expect(actual.unknownCandidateVoxelCount).toBe(expected.unknownCandidateVoxelCount);
  expect(actual.occupancyCellsSaturated).toBe(expected.occupancyCellsSaturated);
  expect(actual.occupancyFineVoxelSizeMeters).toBe(expected.occupancyFineVoxelSizeMeters);
  expect(actual.occupancyDroppedCellUpdates).toBe(expected.occupancyDroppedCellUpdates);

  const bytes = (positions: Float32Array): Uint8Array =>
    new Uint8Array(positions.buffer, positions.byteOffset, positions.byteLength);
  expect(bytes(actual.freeAirPositions)).toEqual(bytes(expected.freeAirPositions));
  expect(bytes(actual.occupiedSurfacePositions)).toEqual(bytes(expected.occupiedSurfacePositions));
  expect(bytes(actual.unknownCandidatePositions)).toEqual(bytes(expected.unknownCandidatePositions));
  return actual;
}

function nearLimitMixedGrid(): { grid: SparseOccupancyGrid; bounds: OccupancyBounds } {
  const grid = new SparseOccupancyGrid(0.1, 50_000);
  const bounds: OccupancyBounds = {
    min: [-12, -12, -12],
    max: [12, 12, 12],
  };
  const stride = 5;
  const half = Math.floor(stride / 2);
  const keys: string[] = [];

  // Bounds contain 241^3 fine cells. The unchanged MAX-213 formula picks
  // stride 5, yielding 49^3 candidate cells (117,649) aligned to world zero.
  outer: for (let cx = -24; cx <= 24; cx += 1) {
    for (let cy = -24; cy <= 24; cy += 1) {
      for (let cz = -24; cz <= 24; cz += 1) {
        keys.push(keyFromCoordinates(cx * stride + half, cy * stride + half, cz * stride + half));
        if (keys.length === 50_000) break outer;
      }
    }
  }

  markClassified(grid, keys.slice(0, 25_000), false);
  markClassified(grid, keys.slice(25_000), true);
  return { grid, bounds };
}

describe('MAX-404 occupancy snapshot allocation and parity', () => {
  it('matches the old output bytes across deterministic occupancy fixtures', () => {
    const empty = new SparseOccupancyGrid(0.1);
    expectSnapshotMatchesOracle(empty, null);

    const onlyFree = new SparseOccupancyGrid(0.1);
    markClassified(onlyFree, ['-3:2:-1', '0:0:0'], false);
    expectSnapshotMatchesOracle(onlyFree, { min: [-0.5, -0.5, -0.5], max: [0.5, 0.5, 0.5] });

    const onlyOccupied = new SparseOccupancyGrid(0.1);
    markClassified(onlyOccupied, ['-4:-2:-6', '1:2:3'], true);
    expectSnapshotMatchesOracle(onlyOccupied, null);

    const unknownStrideOne = new SparseOccupancyGrid(0.1);
    expectSnapshotMatchesOracle(unknownStrideOne, { min: [-0.2, -0.2, -0.2], max: [0.2, 0.2, 0.2] });

    const unknownStrideGreaterThanOne = new SparseOccupancyGrid(0.1);
    const strided = expectSnapshotMatchesOracle(unknownStrideGreaterThanOne, {
      min: [-4, -4, -2],
      max: [4, 4, 2],
    });
    expect(strided.occupancyVoxelSizeMeters).toBeGreaterThan(0.1);

    const negativeCoordinates = new SparseOccupancyGrid(0.1);
    markClassified(negativeCoordinates, ['-17:-9:-3'], false);
    markClassified(negativeCoordinates, ['-15:-8:-2'], true);
    expectSnapshotMatchesOracle(negativeCoordinates, {
      min: [-1.8, -1, -0.4],
      max: [-1.2, -0.5, 0.1],
    });

    const capped = new SparseOccupancyGrid(0.18, 1_000);
    const cappedKeys = Array.from({ length: 1_005 }, (_, index) => `${index - 3}:0:0`);
    markClassified(capped, cappedKeys, false);
    const cappedSnapshot = expectSnapshotMatchesOracle(capped, {
      min: [-0.36, -0.36, -0.36],
      max: [1.8, 1.8, 1.8],
    });
    expect(capped.cellCount).toBe(1_000);
    expect(cappedSnapshot.occupancyCellsSaturated).toBe(true);

    const nearLimit = nearLimitMixedGrid();
    const largeSnapshot = expectSnapshotMatchesOracle(nearLimit.grid, nearLimit.bounds);
    expect(largeSnapshot.knownFreeVoxelCount).toBe(25_000);
    expect(largeSnapshot.occupiedSurfaceVoxelCount).toBe(25_000);
    expect(largeSnapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
    expect(largeSnapshot.occupancyVoxelSizeMeters).toBe(0.5);

    const beyondSafeVolume = new SparseOccupancyGrid(0.1);
    const hugeBounds: OccupancyBounds = {
      min: [-50_000, -50_000, -50_000],
      max: [50_000, 50_000, 50_000],
    };
    const rawAxisCells = Math.floor(hugeBounds.max[0] / 0.1) - Math.floor(hugeBounds.min[0] / 0.1) + 1;
    expect(rawAxisCells ** 3).toBeGreaterThan(Number.MAX_SAFE_INTEGER);
    const hugeSnapshot = expectSnapshotMatchesOracle(beyondSafeVolume, hugeBounds);
    expect(hugeSnapshot.occupancyVoxelSizeMeters).toBeGreaterThan(0.1);
    expect(hugeSnapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);
  }, 15_000);

  it('uses zero coordinate pushes and one exact typed allocation per output array', () => {
    const { grid, bounds } = nearLimitMixedGrid();
    const NativeFloat32Array = globalThis.Float32Array;
    const nativePushDescriptor = Object.getOwnPropertyDescriptor(Array.prototype, 'push');
    if (nativePushDescriptor === undefined) throw new Error('Array.prototype.push descriptor missing');

    let coordinateNumberPushes = 0;
    const constructorSizes: Array<number | string> = [];
    const fromSpy = vi.spyOn(NativeFloat32Array, 'from');
    const instrumentedFloat32Array = new Proxy(NativeFloat32Array, {
      construct(target, args) {
        const length = args[0];
        constructorSizes[constructorSizes.length] = typeof length === 'number'
          ? length
          : `non-number:${typeof length}`;
        return Reflect.construct(target, args, target);
      },
    });

    Object.defineProperty(Array.prototype, 'push', {
      ...nativePushDescriptor,
      value: function (this: unknown[], ...items: unknown[]): number {
        coordinateNumberPushes += items.filter((item) => typeof item === 'number').length;
        return Reflect.apply(nativePushDescriptor.value as (...values: unknown[]) => number, this, items) as number;
      },
    });
    vi.stubGlobal('Float32Array', instrumentedFloat32Array);

    let snapshot: OccupancySnapshot;
    let fromCalls = 0;
    try {
      snapshot = grid.snapshot(bounds);
      fromCalls = fromSpy.mock.calls.length;
    } finally {
      Object.defineProperty(Array.prototype, 'push', nativePushDescriptor);
      fromSpy.mockRestore();
      vi.unstubAllGlobals();
    }

    const outputs = [
      snapshot.freeAirPositions,
      snapshot.occupiedSurfacePositions,
      snapshot.unknownCandidatePositions,
    ];
    const finalLengths = outputs.map((positions) => positions.length);
    console.info('[MAX-404 allocation probe]', JSON.stringify({
      coordinateNumberPushes,
      float32ArrayFromCalls: fromCalls,
      constructorSizes,
      finalLengths,
    }));

    expect({ coordinateNumberPushes, float32ArrayFromCalls: fromCalls, constructorSizes, finalLengths }).toEqual({
      coordinateNumberPushes: 0,
      float32ArrayFromCalls: 0,
      constructorSizes: finalLengths,
      finalLengths,
    });
    expect(finalLengths.every((length) => length > 0)).toBe(true);
    for (const positions of outputs) {
      expect(positions.byteOffset).toBe(0);
      expect(positions.buffer.byteLength).toBe(positions.length * NativeFloat32Array.BYTES_PER_ELEMENT);
    }
  });
});
