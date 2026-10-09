import { describe, expect, it } from 'vitest';

import {
  enumerateUnknownCandidatePositions,
  SparseOccupancyGrid,
  type OccupancyDepthFrameLike,
  type OccupancyFrameContext,
  type OccupancyViewLike,
} from '../../src/xr/occupancyEvidence';
import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import {
  decodeOccupancySolverArtifact,
  encodeOccupancySolverArtifact,
  occupancyCrc32,
} from '../../src/xr/occupancySolverArtifact';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

const FINE_VOXEL_METERS = 0.1;

function projection(focalScale = 1): Float32Array {
  return new Float32Array([
    focalScale, 0, 0, 0,
    0, focalScale, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
  ]);
}

function viewAt(translationX = 0, focalScale = 1): OccupancyViewLike {
  const matrix = new Float32Array([
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    translationX, 0, 0, 1,
  ]);
  return { projectionMatrix: projection(focalScale), transform: { matrix } };
}

function frameContext(
  keyframeId: number,
  cameraX = 0,
  maxDepthMeters = 5,
  frameWeight = 1,
): OccupancyFrameContext {
  return {
    keyframeId,
    evidenceKeyframe: true,
    frameWeight,
    cameraProjectPosition: [cameraX, 0, 0],
    maxDepthMeters,
  };
}

function pointKey(point: readonly number[], size: number): string {
  return point.map((coordinate) => Math.floor(coordinate / size)).join(':');
}

function decodedKeys(values: Int16Array): Set<string> {
  const keys = new Set<string>();
  for (let offset = 0; offset < values.length; offset += 3) {
    keys.add(`${values[offset]}:${values[offset + 1]}:${values[offset + 2]}`);
  }
  return keys;
}

function solverSnapshot(
  freeAirPositions: Float32Array,
  occupiedSurfacePositions: Float32Array,
  fineVoxelSizeMeters: number,
  occupancyVoxelSizeMeters = fineVoxelSizeMeters,
  occupancyCellsSaturated = false,
) {
  return {
    freeAirPositions,
    occupiedSurfacePositions,
    occupancyEnvelopeBounds: { min: [-1, -1, -1] as const, max: [1, 1, 1] as const },
    occupancySolverCandidateBelowSurfaceMeters: 2,
    occupancyCellsSaturated,
    occupancyFineVoxelSizeMeters: fineVoxelSizeMeters,
    occupancyVoxelSizeMeters,
  };
}

function scanSnapshotWithOccupancy(
  occupancy: ReturnType<SparseOccupancyGrid['snapshot']>,
  envelope: { min: readonly [number, number, number]; max: readonly [number, number, number] },
): ScanPointCloudSnapshot {
  return {
    positions: new Float32Array(0),
    confidence: new Float32Array(0),
    normals: new Float32Array(0),
    pointCount: 0,
    tentativePositions: new Float32Array(0),
    tentativeConfidence: new Float32Array(0),
    tentativePointCount: 0,
    rawPointCount: 0,
    bounds: null,
    fullBounds: null,
    voxelSizeMeters: FINE_VOXEL_METERS,
    maxDepthMeters: 5,
    depthFrameCount: 0,
    sampledDepthFrames: 0,
    capacityReached: false,
    maxPointCapacity: 1,
    boundsTrimFraction: 0,
    coverageCellCount: 0,
    coverageCellSizeMeters: FINE_VOXEL_METERS,
    adaptivePointsPerCell: 1,
    memoryBudgetPoints: 1,
    candidateSampleCount: 0,
    discardedSampleCount: 0,
    replacementCount: 0,
    rebalanceEvictionCount: 0,
    coverageRejectedCount: 0,
    promotedPointCount: 0,
    readyCoverageCellCount: 0,
    integrationKeyframes: 0,
    polishFrames: 0,
    residualRejectedSamples: 0,
    staleTentativePruned: 0,
    averageStableResidualMeters: 0,
    ...occupancy,
    occupancyEnvelopeBounds: envelope,
    occupancySolverCandidateBelowSurfaceMeters: 2,
    occupancyEnvelopeComponentCount: 1,
    occupancyQuarantinedSurfaceComponentCount: 0,
    occupancyQuarantinedSurfacePointCount: 0,
    occupancyBridgeRescanComponentCount: 0,
    occupancyBridgeEvidenceTruncated: false,
    tsdfPositions: new Float32Array(0),
    tsdfNormals: new Float32Array(0),
    tsdfConfidence: new Float32Array(0),
    tsdfResidualMeters: new Float32Array(0),
    tsdfNormalConfidence: new Float32Array(0),
    tsdfCoverageConfidence: new Float32Array(0),
    tsdfCoverageReady: new Uint8Array(0),
    tsdfPointCount: 0,
    manualControlPoints: [],
    operatorEdges: [],
  };
}

function rewriteOccupancyHeader(
  bytes: Uint8Array,
  update: (header: Record<string, unknown>) => void,
): Uint8Array {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(8, true);
  const payloadLength = view.getUint32(12, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(20, 20 + headerLength))) as Record<string, unknown>;
  update(header);
  const encodedHeader = new TextEncoder().encode(JSON.stringify(header));
  const result = new Uint8Array(20 + encodedHeader.length + payloadLength);
  result.set(bytes.subarray(0, 20));
  result.set(encodedHeader, 20);
  result.set(bytes.subarray(20 + headerLength), 20 + encodedHeader.length);
  const resultView = new DataView(result.buffer);
  resultView.setUint32(8, encodedHeader.length, true);
  resultView.setUint32(16, occupancyCrc32(result, 20), true);
  return result;
}

function seededNoise(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x100000000;
  };
}

function analyticNoisyScene(): OccupancyDepthFrameLike {
  const width = 160;
  const height = 90;
  const noise = seededNoise(0x213);
  const values = new Float32Array(width * height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const u = (x + 0.5) / width;
      let depth = u < 0.4 ? 1.5 : u < 0.58 ? 1.5 + ((u - 0.4) / 0.18) * 0.2 : 2.0;
      depth += (noise() - 0.5) * 0.008;
      if ((x % 23 === 0 && y % 19 === 0) || (u >= 0.82 && u <= 0.9)) depth = Number.NaN;
      else if (x % 41 === 7 && y % 31 === 11) depth = 4.7;
      else if (Math.abs(u - 0.58) < 0.012) depth = 1.75;
      values[y * width + x] = depth;
    }
  }
  return {
    width,
    height,
    getDepthInMeters: (u, v) => {
      const x = Math.max(0, Math.min(width - 1, Math.floor(u * width)));
      const y = Math.max(0, Math.min(height - 1, Math.floor(v * height)));
      return values[y * width + x];
    },
  };
}

function analyticSurfaceDepth(u: number): number {
  if (u < 0.4) return 1.5;
  if (u < 0.58) return 1.5 + ((u - 0.4) / 0.18) * 0.2;
  return 2.0;
}

function isUnknownAnalyticSample(u: number, v: number, width: number, height: number): boolean {
  if (u >= 0.82 && u <= 0.9 || u < 0 || u >= 1 || v < 0 || v >= 1) return true;
  const x = Math.floor(u * width);
  const y = Math.floor(v * height);
  return (x % 23 === 0 && y % 19 === 0) || (x % 41 === 7 && y % 31 === 11) || Math.abs(u - 0.58) < 0.012;
}

describe('deep occupancy evidence gates', () => {
  it('keeps analytic FREE/OCCUPIED/UNKNOWN states conservative under jitter, mixed pixels, holes, and outliers', () => {
    const depth = analyticNoisyScene();
    const grid = new SparseOccupancyGrid(FINE_VOXEL_METERS, 50000);
    for (let keyframeId = 1; keyframeId <= 8; keyframeId += 1) {
      grid.integrateDepthFrame(depth, viewAt(), frameContext(keyframeId));
    }

    expect(grid.classifyPoint([0.05, 0.35, 0.05])).toBe('free');
    expect(grid.classifyPoint([0.05, 1.65, 0.05])).toBe('occupied');
    expect(grid.classifyPoint([0.05, 2.05, 0.05])).toBe('unknown');
    expect(grid.classifyPoint([0.75, 0.95, 0.05])).toBe('unknown');

    let groundTruthFree = 0;
    let groundTruthNotFree = 0;
    let falseFree = 0;
    let falseOccupied = 0;
    for (let xCell = -8; xCell <= 8; xCell += 1) {
      for (let yCell = 2; yCell <= 26; yCell += 1) {
        for (let zCell = -6; zCell <= 6; zCell += 1) {
          const point = [
            (xCell + 0.5) * FINE_VOXEL_METERS,
            (yCell + 0.5) * FINE_VOXEL_METERS,
            (zCell + 0.5) * FINE_VOXEL_METERS,
          ] as const;
          const u = 0.5 * (1 + point[0] / point[1]);
          const v = 0.5 * (1 - point[2] / point[1]);
          if (isUnknownAnalyticSample(u, v, depth.width, depth.height)) continue;
          const hitDepth = analyticSurfaceDepth(u);
          const classification = grid.classifyPoint(point);
          if (point[1] <= hitDepth - 0.25) {
            groundTruthFree += 1;
            if (classification === 'occupied') falseOccupied += 1;
          } else if (point[1] >= hitDepth - 0.1) {
            groundTruthNotFree += 1;
            if (classification === 'free') falseFree += 1;
          }
        }
      }
    }

    expect(groundTruthFree).toBeGreaterThan(100);
    expect(groundTruthNotFree).toBeGreaterThan(100);
    expect(falseFree / groundTruthNotFree).toBeLessThanOrEqual(0.01);
    expect(falseOccupied / groundTruthFree).toBeLessThanOrEqual(0.02);
  });

  it('round-trips fine voxel volumes independently and keeps strided UNKNOWN centers on their declared coarse grid', () => {
    const freeWorld = [
      [-0.15, -0.05, 0.05],
      [0.05, 0.05, 0.05],
      [0.15, -0.15, -0.05],
    ];
    const occupiedWorld = [
      [-0.05, 0.15, -0.15],
      [0.25, 0.05, 0.05],
    ];
    const snapshot = solverSnapshot(
      new Float32Array(freeWorld.flat()),
      new Float32Array(occupiedWorld.flat()),
      FINE_VOXEL_METERS,
      0.3,
    );
    const options = { frameId: 'deep-geometry', createdAtMs: 1 };
    const bytes = encodeOccupancySolverArtifact(snapshot, options);
    const decoded = decodeOccupancySolverArtifact(bytes);
    const expectedFree = new Set(freeWorld.map((point) => pointKey(point, FINE_VOXEL_METERS)));
    const expectedOccupied = new Set(occupiedWorld.map((point) => pointKey(point, FINE_VOXEL_METERS)));

    expect(decoded.header.voxel_size_m).toBe(FINE_VOXEL_METERS);
    expect(decodedKeys(decoded.free)).toEqual(expectedFree);
    expect(decodedKeys(decoded.occupied)).toEqual(expectedOccupied);
    const rehashedFree = new Set<string>();
    const rehashedOccupied = new Set<string>();
    for (const [cells, rehashed] of [[decoded.free, rehashedFree], [decoded.occupied, rehashedOccupied]] as const) {
      for (let offset = 0; offset < cells.length; offset += 3) {
        const center = [0, 1, 2].map((axis) => (cells[offset + axis] + 0.5) * FINE_VOXEL_METERS);
        rehashed.add(pointKey(center, FINE_VOXEL_METERS));
      }
    }
    expect(rehashedFree).toEqual(expectedFree);
    expect(rehashedOccupied).toEqual(expectedOccupied);
    expect(expectedFree.size * FINE_VOXEL_METERS ** 3).toBeCloseTo(freeWorld.length * FINE_VOXEL_METERS ** 3, 12);
    expect(expectedOccupied.size * FINE_VOXEL_METERS ** 3).toBeCloseTo(occupiedWorld.length * FINE_VOXEL_METERS ** 3, 12);

    const classifyDecoded = (point: readonly number[]): 'free' | 'occupied' | 'unknown' => {
      const key = pointKey(point, decoded.header.voxel_size_m);
      if (expectedFree.has(key)) return 'free';
      if (expectedOccupied.has(key)) return 'occupied';
      return 'unknown';
    };
    expect(classifyDecoded(freeWorld[0])).toBe('free');
    expect(classifyDecoded(occupiedWorld[0])).toBe('occupied');
    expect(classifyDecoded([0.35, 0.05, 0.05])).toBe('unknown');

    const restored = solverSnapshot(
      new Float32Array(Array.from(decoded.free, (index) => (index + 0.5) * FINE_VOXEL_METERS)),
      new Float32Array(Array.from(decoded.occupied, (index) => (index + 0.5) * FINE_VOXEL_METERS)),
      FINE_VOXEL_METERS,
      0.3,
    );
    expect(Array.from(encodeOccupancySolverArtifact(restored, options))).toEqual(Array.from(bytes));

    const translated = solverSnapshot(
      new Float32Array(freeWorld.flatMap(([x, y, z]) => [x + 0.3, y, z])),
      new Float32Array(occupiedWorld.flatMap(([x, y, z]) => [x + 0.3, y, z])),
      FINE_VOXEL_METERS,
      0.3,
    );
    const translatedDecoded = decodeOccupancySolverArtifact(encodeOccupancySolverArtifact(translated, options));
    expect(decodedKeys(translatedDecoded.free)).toEqual(new Set([...expectedFree].map((key) => {
      const [x, y, z] = key.split(':').map(Number);
      return `${x + 3}:${y}:${z}`;
    })));
    expect(decodedKeys(translatedDecoded.occupied)).toEqual(new Set([...expectedOccupied].map((key) => {
      const [x, y, z] = key.split(':').map(Number);
      return `${x + 3}:${y}:${z}`;
    })));

    const bounds = { min: [-8, -8, -4] as const, max: [8, 8, 4] as const };
    const coarse = enumerateUnknownCandidatePositions(bounds, FINE_VOXEL_METERS, () => true);
    const coarseOccupancy = new SparseOccupancyGrid(FINE_VOXEL_METERS).snapshot(bounds);
    const expectedCoarseKeys = new Set<string>();
    for (let x = -27; x <= 26; x += 1) {
      for (let y = -27; y <= 26; y += 1) {
        for (let z = -14; z <= 13; z += 1) expectedCoarseKeys.add(`${x}:${y}:${z}`);
      }
    }
    const actualCoarseKeys = new Set<string>();
    for (let offset = 0; offset < coarse.unknownCandidatePositions.length; offset += 3) {
      const point = [0, 1, 2].map((axis) => coarse.unknownCandidatePositions[offset + axis]);
      const key = point.map((coordinate) => Math.floor(coordinate / coarse.occupancyVoxelSizeMeters)).join(':');
      actualCoarseKeys.add(key);
    }
    expect(coarse.occupancyVoxelSizeMeters).toBeCloseTo(0.3, 12);
    expect(actualCoarseKeys).toEqual(expectedCoarseKeys);
    expect(actualCoarseKeys.size).toBe(coarse.unknownCandidateVoxelCount);
    expect(Array.from(coarseOccupancy.unknownCandidatePositions)).toEqual(Array.from(coarse.unknownCandidatePositions));

    const coarseScan = scanSnapshotWithOccupancy(coarseOccupancy, bounds);
    const coarseRestored = decodeScanArtifact(encodeScanArtifact(coarseScan));
    expect(coarseRestored.occupancyFineVoxelSizeMeters).toBe(FINE_VOXEL_METERS);
    expect(coarseRestored.occupancyVoxelSizeMeters).toBeCloseTo(coarse.occupancyVoxelSizeMeters, 7);
    expect(coarseRestored.unknownCandidateVoxelCount).toBe(coarse.unknownCandidateVoxelCount);
    expect(Array.from(coarseRestored.unknownCandidatePositions)).toEqual(Array.from(coarse.unknownCandidatePositions));
  });

  it('keeps the final classified key sets invariant when the same evidence keyframes arrive in reverse order', () => {
    const depth: OccupancyDepthFrameLike = {
      width: 160,
      height: 90,
      getDepthInMeters: () => 1.5,
    };
    const classifiedKeys = (order: readonly number[]) => {
      const grid = new SparseOccupancyGrid(0.18, 50000);
      for (const keyframeId of order) {
        grid.integrateDepthFrame(depth, viewAt(0, 4), frameContext(keyframeId));
      }
      const snapshot = grid.snapshot(null);
      const keys = (positions: Float32Array) => {
        const result = new Set<string>();
        for (let offset = 0; offset < positions.length; offset += 3) {
          result.add(pointKey([positions[offset], positions[offset + 1], positions[offset + 2]], grid.voxelSize));
        }
        return result;
      };
      return { free: keys(snapshot.freeAirPositions), occupied: keys(snapshot.occupiedSurfacePositions) };
    };

    const forward = classifiedKeys([1, 2, 3, 4, 5, 6]);
    const reverse = classifiedKeys([6, 5, 4, 3, 2, 1]);
    expect(reverse.free).toEqual(forward.free);
    expect(reverse.occupied).toEqual(forward.occupied);
  });

  it('persists every solver key beyond the historical 12000 point display limit', () => {
    const grid = new SparseOccupancyGrid(FINE_VOXEL_METERS, 30000);
    const update = (grid as unknown as {
      updateCell(key: string, occupied: boolean, weight: number, keyframeId: number): void;
    }).updateCell.bind(grid);
    for (let index = 0; index < 13025; index += 1) {
      const x = index - 6512;
      for (let keyframeId = 1; keyframeId <= 3; keyframeId += 1) {
        update(`${x}:0:0`, false, 1, keyframeId);
        update(`${x}:10000:0`, true, 1, keyframeId);
      }
    }

    const occupancy = grid.snapshot(null);
    const snapshot = solverSnapshot(
      occupancy.freeAirPositions,
      occupancy.occupiedSurfacePositions,
      FINE_VOXEL_METERS,
    );
    const bytes = encodeOccupancySolverArtifact(snapshot, { frameId: 'beyond-display-cap', createdAtMs: 2 });
    const decoded = decodeOccupancySolverArtifact(bytes);
    const persistedFree = decodedKeys(decoded.free);
    const persistedOccupied = decodedKeys(decoded.occupied);

    expect(occupancy.knownFreeVoxelCount).toBe(13025);
    expect(occupancy.occupiedSurfaceVoxelCount).toBe(13025);
    expect(decoded.header.free_count).toBe(13025);
    expect(decoded.header.occupied_count).toBe(13025);
    expect(persistedFree.has('6512:0:0')).toBe(true);
    expect(persistedOccupied.has('6512:10000:0')).toBe(true);
    expect(persistedFree.size).toBe(occupancy.knownFreeVoxelCount);
    expect(persistedOccupied.size).toBe(occupancy.occupiedSurfaceVoxelCount);
  });

  it('surfaces dropped evidence, leaves a later scanned region UNKNOWN, and recovers after reset', () => {
    const depth: OccupancyDepthFrameLike = {
      width: 160,
      height: 90,
      getDepthInMeters: () => 3,
    };
    const grid = new SparseOccupancyGrid(0.5, 1000);
    for (let keyframeId = 1; keyframeId <= 8; keyframeId += 1) {
      grid.integrateDepthFrame(depth, viewAt(0, 4), frameContext(keyframeId, 0));
    }
    expect(grid.snapshot(null).occupancyCellsSaturated).toBe(false);

    for (let keyframeId = 9; keyframeId <= 60; keyframeId += 1) {
      const cameraX = 10 + (keyframeId - 9) * 0.75;
      grid.integrateDepthFrame(depth, viewAt(cameraX, 4), frameContext(keyframeId, cameraX));
    }
    const saturated = grid.snapshot({ min: [99.5, 0, -1], max: [101, 3, 1] });
    const omittedCandidate = [100.25, 1.25, 0.25] as const;
    expect(grid.cellCount).toBe(1000);
    expect(saturated.occupancyCellsSaturated).toBe(true);
    expect(saturated.occupancyDroppedCellUpdates).toBeGreaterThan(0);
    expect(grid.classifyPoint(omittedCandidate)).toBe('unknown');
    expect(saturated.unknownCandidateVoxelCount).toBeGreaterThan(0);

    const solverInput = {
      ...saturated,
      occupancyEnvelopeBounds: { min: [99.5, 0, -1] as const, max: [101, 3, 1] as const },
      occupancySolverCandidateBelowSurfaceMeters: 2,
    };
    const saved = decodeOccupancySolverArtifact(encodeOccupancySolverArtifact(solverInput, {
      frameId: 'saturated-session',
      createdAtMs: 3,
    }));
    expect(saved.header.saturated).toBe(true);
    expect(decodedKeys(saved.free).has(pointKey(omittedCandidate, 0.5))).toBe(false);
    expect(decodedKeys(saved.occupied).has(pointKey(omittedCandidate, 0.5))).toBe(false);

    grid.reset();
    for (let keyframeId = 1; keyframeId <= 8; keyframeId += 1) {
      grid.integrateDepthFrame(depth, viewAt(100, 4), frameContext(keyframeId, 100));
    }
    expect(grid.snapshot(null).occupancyCellsSaturated).toBe(false);
    expect(grid.classifyPoint(omittedCandidate)).toBe('free');
  });

  it('fails closed for truncated bytes, checksum damage, unsupported schema, and count/payload disagreement', () => {
    const snapshot = solverSnapshot(
      new Float32Array([0.05, 0.05, 0.05, 0.15, 0.05, 0.05]),
      new Float32Array([-0.05, 0.15, 0.05]),
      FINE_VOXEL_METERS,
    );
    const bytes = encodeOccupancySolverArtifact(snapshot, { frameId: 'corruption-corpus', createdAtMs: 4 });
    const truncated = bytes.slice(0, -1);
    const checksumDamage = bytes.slice();
    checksumDamage[checksumDamage.length - 1] ^= 0xff;
    const unsupportedSchema = rewriteOccupancyHeader(bytes, (header) => { header.schema = 99; });
    const countMismatch = rewriteOccupancyHeader(bytes, (header) => { header.free_count = 1; });

    expect(() => decodeOccupancySolverArtifact(truncated)).toThrow();
    expect(() => decodeOccupancySolverArtifact(checksumDamage)).toThrow(/checksum/i);
    expect(() => decodeOccupancySolverArtifact(unsupportedSchema)).toThrow(/schema/i);
    expect(() => decodeOccupancySolverArtifact(countMismatch)).toThrow(/counts/i);
  });
});
