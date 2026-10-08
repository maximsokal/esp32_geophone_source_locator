import { describe, expect, it } from 'vitest';

import {
  decodeOccupancySolverArtifact,
  encodeOccupancySolverArtifact,
} from '../../src/xr/occupancySolverArtifact';
import { decodeScanArtifact, encodeScanArtifact, MAX_SCAN_ARTIFACT_BYTES } from '../../src/xr/scanArtifact';
import {
  buildContinuationSpatialHash,
  nearestContinuationPoint,
} from '../../src/xr/continuationSpatialIndex';
import { registerAndMergeScanContinuation } from '../../src/xr/scanContinuation';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

type ContinuationOccupancyFixture = Partial<Pick<ScanPointCloudSnapshot,
  'freeAirPositions' | 'occupiedSurfacePositions' | 'unknownCandidatePositions' |
  'occupancyVoxelSizeMeters' | 'occupancyFineVoxelSizeMeters' | 'occupancyEnvelopeBounds' |
  'occupancySolverCandidateBelowSurfaceMeters' | 'occupancyCellsSaturated' | 'occupancyDroppedCellUpdates' |
  'occupancyEnvelopeComponentCount' | 'occupancyQuarantinedSurfaceComponentCount' |
  'occupancyQuarantinedSurfacePointCount' | 'occupancyBridgeRescanComponentCount' |
  'occupancyBridgeEvidenceTruncated'>>;
type ContinuationAnnotations = Partial<Pick<ScanPointCloudSnapshot,
  'normalConfidence' | 'manualControlPoints' | 'operatorEdges'>>;

function cloudPoints(count = 240): Float32Array {
  const points = new Float32Array(count * 3);
  for (let index = 0; index < count; index += 1) {
    const xCell = index % 20;
    const yCell = Math.floor(index / 20);
    const offset = index * 3;
    points[offset] = xCell * 0.075 + 0.012 * Math.sin(yCell * 0.7);
    points[offset + 1] = yCell * 0.085 + 0.018 * Math.sin(xCell * 0.43);
    points[offset + 2] = 0.18 * Math.sin(xCell * 0.31) + 0.11 * Math.cos(yCell * 0.53) + 0.015 * yCell;
  }
  return points;
}

function periodicGridPoints(): Float32Array {
  const points = new Float32Array(20 * 20 * 3);
  let index = 0;
  for (let x = 0; x < 20; x += 1) {
    for (let y = 0; y < 20; y += 1) {
      const offset = index++ * 3;
      points[offset] = x * 0.1;
      points[offset + 1] = y * 0.1;
      points[offset + 2] = 0;
    }
  }
  return points;
}

function partialOverlapPair(
  appendedLengthMeters: number,
  yaw = 23 * Math.PI / 180,
  translation: readonly [number, number, number] = [0.8, -0.5, 0.3],
): {
  base: ScanPointCloudSnapshot;
  extension: ScanPointCloudSnapshot;
  yaw: number;
  translation: readonly [number, number, number];
} {
  const world: number[] = [];
  const overlap: number[] = [];
  for (let xIndex = 0; xIndex < 30; xIndex += 1) {
    for (let yIndex = 0; yIndex < 20; yIndex += 1) {
      const x = (2 * xIndex) / 29;
      const y = (2 * yIndex) / 19;
      const z = 0.18 * Math.sin(xIndex * 0.43 + yIndex * 0.67) +
        0.13 * Math.cos(xIndex * 0.83 - yIndex * 0.29) +
        0.04 * Math.sin(xIndex * yIndex * 0.17);
      world.push(x, y, z);
      if (xIndex >= 24) overlap.push(x, y, z);
    }
  }
  if (appendedLengthMeters > 0) {
    for (let index = 0; index < 300; index += 1) {
      const fraction = (value: number): number => {
        const scaled = Math.sin(value * 12.9898) * 43_758.5453;
        return scaled - Math.floor(scaled);
      };
      const x = 5 + fraction(index * 3 + 1) * appendedLengthMeters;
      const y = 3 + fraction(index * 7 + 4) * 2;
      const z = 1.2 + fraction(index * 11 + 9) * 1.2;
      overlap.push(x, y, z);
    }
  }
  return {
    base: snapshot(new Float32Array(world)),
    extension: snapshot(transformIntoSession(new Float32Array(overlap), yaw, translation)),
    yaw,
    translation,
  };
}

function normalsFor(points: Float32Array): Float32Array {
  const normals = new Float32Array(points.length);
  for (let offset = 0; offset < normals.length; offset += 3) {
    normals[offset] = 0;
    normals[offset + 1] = 0;
    normals[offset + 2] = 1;
  }
  return normals;
}

function confidenceFor(points: Float32Array): Float32Array {
  const confidence = new Float32Array(points.length / 3);
  confidence.fill(0.92);
  return confidence;
}

function transformIntoSession(
  world: Float32Array,
  yaw: number,
  translation: readonly [number, number, number],
): Float32Array {
  // Create session-local coordinates such that the tested rigid transform maps
  // them back to the world/saved frame: world = R(yaw) * local + t.
  const result = new Float32Array(world.length);
  const c = Math.cos(yaw);
  const s = Math.sin(yaw);
  for (let offset = 0; offset < world.length; offset += 3) {
    const wx = world[offset] - translation[0];
    const wy = world[offset + 1] - translation[1];
    result[offset] = c * wx + s * wy;
    result[offset + 1] = -s * wx + c * wy;
    result[offset + 2] = world[offset + 2] - translation[2];
  }
  return result;
}

function offsetPositions(points: Float32Array, offset: readonly [number, number, number]): Float32Array {
  const result = points.slice();
  for (let index = 0; index < result.length; index += 3) {
    result[index] += offset[0];
    result[index + 1] += offset[1];
    result[index + 2] += offset[2];
  }
  return result;
}

function boundsFor(points: Float32Array): ScanPointCloudSnapshot['bounds'] {
  let minX = Number.POSITIVE_INFINITY, minY = Number.POSITIVE_INFINITY, minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY, maxY = Number.NEGATIVE_INFINITY, maxZ = Number.NEGATIVE_INFINITY;
  for (let offset = 0; offset < points.length; offset += 3) {
    minX = Math.min(minX, points[offset]); minY = Math.min(minY, points[offset + 1]); minZ = Math.min(minZ, points[offset + 2]);
    maxX = Math.max(maxX, points[offset]); maxY = Math.max(maxY, points[offset + 1]); maxZ = Math.max(maxZ, points[offset + 2]);
  }
  return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

function snapshot(
  positions: Float32Array,
  occupancy: ContinuationOccupancyFixture & ContinuationAnnotations = {},
): ScanPointCloudSnapshot {
  const confidence = confidenceFor(positions);
  const normals = normalsFor(positions);
  const freeAirPositions = occupancy.freeAirPositions ?? new Float32Array(0);
  const occupiedSurfacePositions = occupancy.occupiedSurfacePositions ?? new Float32Array(0);
  const unknownCandidatePositions = occupancy.unknownCandidatePositions ?? new Float32Array(0);
  return {
    positions,
    confidence,
    normals,
    ...(occupancy.normalConfidence === undefined ? {} : { normalConfidence: occupancy.normalConfidence }),
    pointCount: positions.length / 3,
    tentativePositions: new Float32Array(0),
    tentativeConfidence: new Float32Array(0),
    tentativePointCount: 0,
    rawPointCount: positions.length / 3,
    bounds: boundsFor(positions),
    fullBounds: boundsFor(positions),
    voxelSizeMeters: 0.04,
    maxDepthMeters: 5,
    depthFrameCount: 100,
    sampledDepthFrames: 40,
    capacityReached: false,
    maxPointCapacity: 60000,
    boundsTrimFraction: 0,
    coverageCellCount: 20,
    coverageCellSizeMeters: 0.3,
    adaptivePointsPerCell: 28,
    memoryBudgetPoints: 60000,
    candidateSampleCount: 1000,
    discardedSampleCount: 100,
    replacementCount: 10,
    rebalanceEvictionCount: 0,
    coverageRejectedCount: 0,
    promotedPointCount: positions.length / 3,
    readyCoverageCellCount: 12,
    integrationKeyframes: 20,
    polishFrames: 5,
    residualRejectedSamples: 50,
    staleTentativePruned: 3,
    averageStableResidualMeters: 0.012,
    occupancyVoxelSizeMeters: occupancy.occupancyVoxelSizeMeters ?? 0.18,
    occupancyFineVoxelSizeMeters: occupancy.occupancyFineVoxelSizeMeters,
    occupancySolverCandidateBelowSurfaceMeters: occupancy.occupancySolverCandidateBelowSurfaceMeters,
    knownFreeVoxelCount: freeAirPositions.length / 3,
    occupiedSurfaceVoxelCount: occupiedSurfacePositions.length / 3,
    unknownCandidateVoxelCount: unknownCandidatePositions.length / 3,
    freeAirPositions,
    occupiedSurfacePositions,
    unknownCandidatePositions,
    occupancyEnvelopeBounds: occupancy.occupancyEnvelopeBounds === undefined
      ? boundsFor(positions)
      : occupancy.occupancyEnvelopeBounds,
    occupancyEnvelopeComponentCount: occupancy.occupancyEnvelopeComponentCount ?? 1,
    occupancyQuarantinedSurfaceComponentCount: occupancy.occupancyQuarantinedSurfaceComponentCount ?? 0,
    occupancyQuarantinedSurfacePointCount: occupancy.occupancyQuarantinedSurfacePointCount ?? 0,
    occupancyBridgeRescanComponentCount: occupancy.occupancyBridgeRescanComponentCount ?? 0,
    occupancyBridgeEvidenceTruncated: occupancy.occupancyBridgeEvidenceTruncated ?? false,
    occupancyCellsSaturated: occupancy.occupancyCellsSaturated,
    occupancyDroppedCellUpdates: occupancy.occupancyDroppedCellUpdates,
    tsdfPositions: positions.slice(),
    tsdfNormals: normals.slice(),
    tsdfConfidence: confidence.slice(),
    tsdfResidualMeters: new Float32Array(confidence.length).fill(0.012),
    tsdfNormalConfidence: new Float32Array(confidence.length).fill(0.9),
    tsdfCoverageConfidence: new Float32Array(confidence.length).fill(0.9),
    tsdfCoverageReady: new Uint8Array(confidence.length).fill(1),
    tsdfPointCount: confidence.length,
    manualControlPoints: occupancy.manualControlPoints ?? [],
    operatorEdges: occupancy.operatorEdges ?? [],
  };
}

function hasVoxel(points: Float32Array, target: readonly [number, number, number], voxel = 0.18): boolean {
  const tx = Math.floor(target[0] / voxel), ty = Math.floor(target[1] / voxel), tz = Math.floor(target[2] / voxel);
  for (let offset = 0; offset < points.length; offset += 3) {
    if (
      Math.floor(points[offset] / voxel) === tx &&
      Math.floor(points[offset + 1] / voxel) === ty &&
      Math.floor(points[offset + 2] / voxel) === tz
    ) return true;
  }
  return false;
}

function pointIndexInVoxel(points: Float32Array, target: readonly [number, number, number], voxel: number): number {
  const cell = target.map((value) => Math.floor(value / voxel));
  for (let index = 0; index < points.length / 3; index += 1) {
    const offset = index * 3;
    if (
      Math.floor(points[offset] / voxel) === cell[0] &&
      Math.floor(points[offset + 1] / voxel) === cell[1] &&
      Math.floor(points[offset + 2] / voxel) === cell[2]
    ) return index;
  }
  return -1;
}

function setLargeTsdfEvidence(snapshotValue: ScanPointCloudSnapshot, count: number, y: number): void {
  snapshotValue.tsdfPositions = new Float32Array(count * 3);
  snapshotValue.tsdfNormals = new Float32Array(count * 3);
  snapshotValue.tsdfConfidence = new Float32Array(count).fill(0.82);
  snapshotValue.tsdfResidualMeters = new Float32Array(count).fill(0.018);
  snapshotValue.tsdfNormalConfidence = new Float32Array(count).fill(0.85);
  snapshotValue.tsdfCoverageConfidence = new Float32Array(count).fill(0.8);
  snapshotValue.tsdfCoverageReady = new Uint8Array(count).fill(1);
  snapshotValue.tsdfPointCount = count;
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    snapshotValue.tsdfPositions[offset] = index * 0.05;
    snapshotValue.tsdfPositions[offset + 1] = y;
    snapshotValue.tsdfNormals[offset + 2] = 1;
  }
}

function setOccupancyEvidence(snapshotValue: ScanPointCloudSnapshot, y: number): void {
  const count = 1_000;
  snapshotValue.freeAirPositions = new Float32Array(count * 3);
  snapshotValue.occupiedSurfacePositions = new Float32Array(count * 3);
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    snapshotValue.freeAirPositions[offset] = index * 0.2;
    snapshotValue.freeAirPositions[offset + 1] = y;
    snapshotValue.occupiedSurfacePositions[offset] = index * 0.2;
    snapshotValue.occupiedSurfacePositions[offset + 1] = y + 1;
  }
  snapshotValue.knownFreeVoxelCount = count;
  snapshotValue.occupiedSurfaceVoxelCount = count;
}

function classifyDecodedOccupancy(
  decoded: ReturnType<typeof decodeOccupancySolverArtifact>,
  point: readonly [number, number, number],
): 'free' | 'occupied' | 'unknown' {
  const cell = point.map((value) => Math.floor(value / decoded.header.voxel_size_m));
  const contains = (indices: Int16Array): boolean => {
    for (let offset = 0; offset < indices.length; offset += 3) {
      if (indices[offset] === cell[0] && indices[offset + 1] === cell[1] && indices[offset + 2] === cell[2]) return true;
    }
    return false;
  };
  if (contains(decoded.occupied)) return 'occupied';
  if (contains(decoded.free)) return 'free';
  return 'unknown';
}

function sortedIndices(values: Int16Array): number[] {
  const triples: number[][] = [];
  for (let offset = 0; offset < values.length; offset += 3) {
    triples.push([values[offset], values[offset + 1], values[offset + 2]]);
  }
  triples.sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2]);
  return triples.flat();
}

describe('saved 3D scan continuation', () => {
  it('searches a valid two-cell correspondence and keeps the Euclidean radius authoritative', () => {
    const target = { x: 0.241, y: 0, z: 0, sourceIndex: 0 };
    const hash = buildContinuationSpatialHash([target], 0.12);
    const inside = nearestContinuationPoint(
      { x: 0.119, y: 0, z: 0, sourceIndex: 1 }, hash, 0.12, 0.18,
    );
    expect(inside?.point).toBe(target);
    expect(inside?.distanceSquared).toBeCloseTo(0.122 ** 2, 12);

    const outside = buildContinuationSpatialHash([{ ...target, x: 0.3005 }], 0.12);
    expect(nearestContinuationPoint(
      { x: 0.119, y: 0, z: 0, sourceIndex: 1 }, outside, 0.12, 0.18,
    )).toBeNull();
  });

  it('keeps valid radius membership invariant across registration-grid origins', () => {
    const source = { x: 0.119, y: -0.037, z: 0.091, sourceIndex: 1 };
    const target = { x: 0.25, y: 0.024, z: 0.142, sourceIndex: 0 };
    const expectedDistanceSquared = 0.131 ** 2 + 0.061 ** 2 + 0.051 ** 2;
    for (const ox of [0, 0.04, 0.08, 0.119]) {
      for (const oy of [0, 0.04, 0.08, 0.119]) {
        for (const oz of [0, 0.04, 0.08, 0.119]) {
          const translatedTarget = { ...target, x: target.x + ox, y: target.y + oy, z: target.z + oz };
          const translatedSource = { ...source, x: source.x + ox, y: source.y + oy, z: source.z + oz };
          const match = nearestContinuationPoint(
            translatedSource,
            buildContinuationSpatialHash([translatedTarget], 0.12),
            0.12,
            0.18,
          );
          expect(match?.point.sourceIndex).toBe(0);
          expect(match?.distanceSquared).toBeCloseTo(expectedDistanceSquared, 12);
        }
      }
    }
  });

  it('keeps registration acceptance, inlier count, and RMS invariant under grid-origin shifts', () => {
    const fixture = partialOverlapPair(0);
    const reference = registerAndMergeScanContinuation(fixture.base, fixture.extension);
    expect(reference.ok).toBe(true);
    if (!reference.ok) return;

    for (const offset of [[0.037, 0.071, 0.113], [0.119, 0.041, 0.083]] as const) {
      const shiftedBase = snapshot(offsetPositions(fixture.base.positions, offset));
      const shiftedExtension = snapshot(offsetPositions(fixture.extension.positions, offset));
      const shifted = registerAndMergeScanContinuation(shiftedBase, shiftedExtension);
      expect(shifted.ok).toBe(true);
      if (!shifted.ok) continue;
      expect(shifted.diagnostics.inliers).toBe(reference.diagnostics.inliers);
      expect(shifted.diagnostics.rmsMeters).toBeCloseTo(reference.diagnostics.rmsMeters, 6);
      expect(shifted.extensionToBase.yaw).toBeCloseTo(reference.extensionToBase.yaw, 6);
      const cosine = Math.cos(reference.extensionToBase.yaw);
      const sine = Math.sin(reference.extensionToBase.yaw);
      expect(shifted.extensionToBase.tx).toBeCloseTo(
        reference.extensionToBase.tx + offset[0] - (cosine * offset[0] - sine * offset[1]), 6,
      );
      expect(shifted.extensionToBase.ty).toBeCloseTo(
        reference.extensionToBase.ty + offset[1] - (sine * offset[0] + cosine * offset[1]), 6,
      );
      expect(shifted.extensionToBase.tz).toBeCloseTo(reference.extensionToBase.tz, 6);
    }
  });

  it('rejects a periodic overlap when a materially different transform scores equally', () => {
    const grid = periodicGridPoints();
    const result = registerAndMergeScanContinuation(snapshot(grid), snapshot(grid));
    expect(result.ok, result.ok ? JSON.stringify({ diagnostics: result.diagnostics, transform: result.extensionToBase }) : result.reason).toBe(false);
    if (!result.ok) {
      expect(result.diagnostics?.rejectionReason).toBe('ambiguous_overlap');
      expect(result.reason).toContain('more overlap');
      expect(result.reason).toContain('distinctive geometry');
    }
  });

  it('accepts the periodic overlap after a distinctive anchor breaks the symmetry', () => {
    const grid = periodicGridPoints();
    const anchored = new Float32Array(grid.length + 3);
    anchored.set(grid);
    anchored.set([2.3, 1.7, 0.6], grid.length);
    const result = registerAndMergeScanContinuation(snapshot(anchored), snapshot(anchored));
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.diagnostics.inliers).toBe(anchored.length / 3);
      expect(result.diagnostics.secondBestMargin?.inlierDifference).toBeGreaterThan(0);
    }
  });

  it('rejects a narrow horizontal strip before arbitrary yaw can be accepted', () => {
    const strip = new Float32Array(144 * 3);
    for (let index = 0; index < 144; index += 1) {
      strip[index * 3] = index * 0.02;
      strip[index * 3 + 1] = 0.4;
      strip[index * 3 + 2] = 0.02 * Math.sin(index * 0.4);
    }
    const result = registerAndMergeScanContinuation(snapshot(strip), snapshot(strip));
    expect(result.ok, result.ok ? JSON.stringify({ diagnostics: result.diagnostics, transform: result.extensionToBase }) : result.reason).toBe(false);
    if (!result.ok) {
      expect(result.diagnostics?.rejectionReason).toBe('insufficient_conditioning');
      expect(result.diagnostics?.conditioning?.horizontalMinorVarianceMeters2).toBeLessThan(0.0025);
    }
  });

  it('initializes from partial-overlap translation votes despite large appended geometry', () => {
    let referenceTransform: readonly [number, number, number] | null = null;
    for (const appendedLength of [0, 1, 2, 4]) {
      const fixture = partialOverlapPair(appendedLength);
      const result = registerAndMergeScanContinuation(fixture.base, fixture.extension);
      expect(result.ok, `appended=${appendedLength} m, result=${JSON.stringify(result.ok ? result.diagnostics : { reason: result.reason, diagnostics: result.diagnostics })}`).toBe(true);
      if (!result.ok) continue;
      expect(Math.abs(result.extensionToBase.yaw - fixture.yaw), JSON.stringify({ appendedLength, transform: result.extensionToBase, diagnostics: result.diagnostics })).toBeLessThan(0.05);
      expect(result.extensionToBase.tx).toBeCloseTo(fixture.translation[0], 1);
      expect(result.extensionToBase.ty).toBeCloseTo(fixture.translation[1], 1);
      expect(result.extensionToBase.tz).toBeCloseTo(fixture.translation[2], 1);
      expect(result.diagnostics.inliers).toBeGreaterThanOrEqual(90);
      expect(result.diagnostics.overlapRatio).toBeGreaterThanOrEqual(0.18);
      const current = [result.extensionToBase.tx, result.extensionToBase.ty, result.extensionToBase.tz] as const;
      if (referenceTransform === null) referenceTransform = current;
      else expect(Math.hypot(current[0] - referenceTransform[0], current[1] - referenceTransform[1], current[2] - referenceTransform[2])).toBeLessThan(0.08);
      expect(result.diagnostics.runtimeOperations?.yawHypotheses).toBe(72);
      expect(result.diagnostics.runtimeOperations?.translationVotes).toBeLessThanOrEqual(72 * (160 * 160 + 900 * 4));
      expect(result.diagnostics.runtimeOperations?.scoredHypotheses).toBeLessThanOrEqual(72 * (10 + 6));
      expect(result.diagnostics.runtimeOperations?.refinedHypotheses).toBeLessThanOrEqual(13);
      expect(result.diagnostics.runtimeOperations?.coarseScratchBytes).toBe(2_723_840);
      expect(result.diagnostics.runtimeOperations?.descriptorScratchBytes).toBe(28_800);
      expect(result.diagnostics.runtimeOperations?.localDescriptorDistanceChecks).toBeLessThanOrEqual(2 * 900 * 899);
    }
  });

  it('converges from many 14 cm partial-overlap matches across nominal 12 cm cells', () => {
    const fixture = partialOverlapPair(1, 0, [0.14, 0, 0]);
    const result = registerAndMergeScanContinuation(fixture.base, fixture.extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(Math.abs(result.extensionToBase.yaw)).toBeLessThan(0.01);
    expect(result.extensionToBase.tx).toBeCloseTo(0.14, 2);
    expect(result.extensionToBase.ty).toBeCloseTo(0, 2);
    expect(result.extensionToBase.tz).toBeCloseTo(0, 2);
    expect(result.diagnostics.inliers).toBeGreaterThanOrEqual(90);
    expect(result.diagnostics.rmsMeters).toBeLessThan(0.11);
  });

  it('registers a gravity-aligned session using yaw + translation and survives artifact round-trip', () => {
    const world = cloudPoints();
    const yaw = 27 * Math.PI / 180;
    const translation: readonly [number, number, number] = [0.43, -0.28, 0.17];
    const base = snapshot(world);
    const extension = snapshot(transformIntoSession(world, yaw, translation));

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.diagnostics.inliers).toBeGreaterThanOrEqual(180);
    expect(result.diagnostics.overlapRatio).toBeGreaterThan(0.7);
    expect(result.diagnostics.rmsMeters).toBeLessThan(0.05);
    expect(result.snapshot.pointCount).toBeGreaterThanOrEqual(base.pointCount * 0.85);
    // MAX-416: the accepted B->A transform is returned explicitly, not reconstructed from diagnostics.
    expect(result.extensionToBase.yaw).toBeCloseTo(result.diagnostics.yawRadians, 12);
    expect([result.extensionToBase.tx, result.extensionToBase.ty, result.extensionToBase.tz]).toEqual([...result.diagnostics.translation]);
    expect(Math.abs(Math.abs(result.extensionToBase.yaw) - yaw), JSON.stringify({ transform: result.extensionToBase, diagnostics: result.diagnostics })).toBeLessThan(0.05);

    const restored = decodeScanArtifact(encodeScanArtifact(result.snapshot));
    expect(restored.pointCount).toBe(result.snapshot.pointCount);
    expect(restored.knownFreeVoxelCount).toBe(result.snapshot.knownFreeVoxelCount);
    expect(restored.unknownCandidateVoxelCount).toBe(result.snapshot.unknownCandidateVoxelCount);
  });

  it('rejects an unrelated continuation instead of silently mixing coordinate frames', () => {
    const base = snapshot(cloudPoints());
    const unrelated = cloudPoints();
    for (let offset = 0; offset < unrelated.length; offset += 3) {
      unrelated[offset] = unrelated[offset] * 1.7 + 4.0;
      unrelated[offset + 1] = unrelated[offset + 1] * 0.55 - 3.0;
      unrelated[offset + 2] = unrelated[offset + 2] * 2.2 + 1.3;
    }
    const result = registerAndMergeScanContinuation(base, snapshot(unrelated));
    expect(result.ok, result.ok ? JSON.stringify({ diagnostics: result.diagnostics, transform: result.extensionToBase }) : result.reason).toBe(false);
  });

  it('keeps OCCUPIED authoritative over FREE and UNKNOWN when two accepted scans disagree', () => {
    const geometry = cloudPoints();
    const conflict: readonly [number, number, number] = [2.02, 2.02, 0.54];
    const base = snapshot(geometry, {
      freeAirPositions: new Float32Array(conflict),
      unknownCandidatePositions: new Float32Array(conflict),
    });
    const extension = snapshot(geometry, {
      occupiedSurfacePositions: new Float32Array(conflict),
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(hasVoxel(result.snapshot.occupiedSurfacePositions, conflict)).toBe(true);
    expect(hasVoxel(result.snapshot.freeAirPositions, conflict)).toBe(false);
    expect(hasVoxel(result.snapshot.unknownCandidatePositions, conflict)).toBe(false);
  });

  it('refuses continuation when saved occupancy arrays are truncated', () => {
    const base = snapshot(cloudPoints());
    base.knownFreeVoxelCount = 5;
    base.freeAirPositions = new Float32Array([0, 0, 0]);
    const result = registerAndMergeScanContinuation(base, snapshot(cloudPoints()));
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toContain('display-truncated');
  });

  it('preserves per-surfel normal confidence through reload and keeps the base on equal-quality overlap', () => {
    const basePoints = cloudPoints();
    const extensionPoints = new Float32Array(basePoints.length + 3);
    extensionPoints.set(basePoints);
    extensionPoints.set([4, 4, 4], basePoints.length);
    const base = snapshot(basePoints, { normalConfidence: new Float32Array(basePoints.length / 3).fill(0.35) });
    const extensionConfidence = new Float32Array(extensionPoints.length / 3).fill(0.8);
    extensionConfidence[extensionConfidence.length - 1] = 0.97;
    const extension = snapshot(extensionPoints, { normalConfidence: extensionConfidence });
    const loadedBase = decodeScanArtifact(encodeScanArtifact(base));
    const loadedExtension = decodeScanArtifact(encodeScanArtifact(extension));

    const result = registerAndMergeScanContinuation(loadedBase, loadedExtension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.normalConfidence).toBeInstanceOf(Float32Array);
    expect(result.snapshot.normalConfidence).toHaveLength(result.snapshot.pointCount);
    const overlappingIndex = pointIndexInVoxel(result.snapshot.positions, [0, 0, 0.1], 0.04);
    const addedIndex = pointIndexInVoxel(result.snapshot.positions, [4, 4, 4], 0.04);
    expect(result.snapshot.normalConfidence?.[overlappingIndex]).toBeCloseTo(0.35);
    expect(result.snapshot.normalConfidence?.[addedIndex]).toBeCloseTo(0.97);
    const roundTrip = decodeScanArtifact(encodeScanArtifact(result.snapshot));
    expect(roundTrip.normalConfidence).toEqual(result.snapshot.normalConfidence);
  });

  it('prefers known normal confidence and uses zero only for a sample without it', () => {
    const points = cloudPoints();
    const withConfidence = snapshot(points, { normalConfidence: new Float32Array(points.length / 3).fill(0.6) });
    const withoutConfidence = snapshot(points);
    const missingBase = registerAndMergeScanContinuation(withoutConfidence, withConfidence);
    expect(missingBase.ok).toBe(true);
    if (missingBase.ok) {
      expect(missingBase.snapshot.normalConfidence).toHaveLength(missingBase.snapshot.pointCount);
      expect(missingBase.snapshot.normalConfidence?.every((value) => Math.abs(value - 0.6) < 1e-6)).toBe(true);
    }
    const extensionPoints = new Float32Array(points.length + 3);
    extensionPoints.set(points);
    extensionPoints.set([4, 4, 4], points.length);
    const missingExtension = registerAndMergeScanContinuation(withConfidence, snapshot(extensionPoints));
    expect(missingExtension.ok).toBe(true);
    if (missingExtension.ok) {
      expect(missingExtension.snapshot.normalConfidence).toHaveLength(missingExtension.snapshot.pointCount);
      const knownIndex = pointIndexInVoxel(missingExtension.snapshot.positions, [0, 0, 0.1], 0.04);
      const unknownIndex = pointIndexInVoxel(missingExtension.snapshot.positions, [4, 4, 4], 0.04);
      expect(missingExtension.snapshot.normalConfidence?.[knownIndex]).toBeCloseTo(0.6);
      expect(missingExtension.snapshot.normalConfidence?.[unknownIndex]).toBe(0);
    }
  });

  it('chooses one higher-quality surface sample atomically and lets the base win quality ties', () => {
    const basePoints = cloudPoints();
    const extensionPoints = new Float32Array(basePoints.length + 3);
    extensionPoints.set(basePoints);
    extensionPoints.set([0.03, 0.02, 0.12], basePoints.length);
    const base = snapshot(basePoints, { normalConfidence: new Float32Array(basePoints.length / 3) });
    const extensionNormalConfidence = new Float32Array(extensionPoints.length / 3);
    extensionNormalConfidence[extensionNormalConfidence.length - 1] = 0.95;
    const extension = snapshot(extensionPoints, { normalConfidence: extensionNormalConfidence });
    base.confidence[0] = 0.55;
    base.normals.set([1, 0, 0], 0);
    extension.confidence[extension.confidence.length - 1] = 0.95;
    extension.normals.set([0, 1, 0], extension.normals.length - 3);

    const betterExtension = registerAndMergeScanContinuation(base, extension);
    expect(betterExtension.ok).toBe(true);
    if (betterExtension.ok) {
      const index = pointIndexInVoxel(betterExtension.snapshot.positions, [0, 0, 0.1], 0.04);
      expect(index).toBeGreaterThanOrEqual(0);
      for (const [axis, expected] of [0.03, 0.02, 0.12].entries()) {
        expect(betterExtension.snapshot.positions[index * 3 + axis]).toBeCloseTo(expected);
      }
      expect(betterExtension.snapshot.confidence[index]).toBeCloseTo(0.95);
      expect(Array.from(betterExtension.snapshot.normals.slice(index * 3, index * 3 + 3))).toEqual([0, 1, 0]);
      expect(betterExtension.snapshot.normalConfidence?.[index]).toBeCloseTo(0.95);
    }

    base.confidence[0] = 0.99;
    base.normalConfidence![0] = 0.99;
    base.normals.set([0, 0, 1], 0);
    extension.confidence[extension.confidence.length - 1] = 0.55;
    extensionNormalConfidence[extensionNormalConfidence.length - 1] = 0;
    extension.normals.set([0, 1, 0], extension.normals.length - 3);
    const betterBase = registerAndMergeScanContinuation(base, extension);
    expect(betterBase.ok).toBe(true);
    if (betterBase.ok) {
      const index = pointIndexInVoxel(betterBase.snapshot.positions, [0, 0, 0.1], 0.04);
      expect(betterBase.snapshot.positions[index * 3]).toBeCloseTo(0);
      expect(betterBase.snapshot.positions[index * 3 + 1]).toBeCloseTo(0);
      expect(betterBase.snapshot.positions[index * 3 + 2]).toBeCloseTo(0.11);
      expect(Array.from(betterBase.snapshot.normals.slice(index * 3, index * 3 + 3))).toEqual([0, 0, 1]);
      expect(betterBase.snapshot.normalConfidence?.[index]).toBeCloseTo(0.99);
    }
  });

  it('keeps equal-quality crease samples atomic and independent of insertion order', () => {
    const basePoints = cloudPoints();
    const base = snapshot(basePoints, { normalConfidence: new Float32Array(basePoints.length / 3) });
    const makeExtension = (reverse: boolean): ScanPointCloudSnapshot => {
      const points = new Float32Array(basePoints.length + 6);
      points.set(basePoints);
      const samples = reverse
        ? [[0.01, 0.03, 0.02], [0.02, 0.02, 0.01]]
        : [[0.02, 0.02, 0.01], [0.01, 0.03, 0.02]];
      points.set(samples.flat(), basePoints.length);
      const normalConfidence = new Float32Array(points.length / 3);
      normalConfidence.fill(0.8, basePoints.length / 3);
      const result = snapshot(points, { normalConfidence });
      result.confidence.fill(0.9, basePoints.length / 3);
      result.normals.set(reverse ? [0, -1, 0, 0, 1, 0] : [0, 1, 0, 0, -1, 0], basePoints.length);
      return result;
    };
    const first = registerAndMergeScanContinuation(base, makeExtension(false));
    const second = registerAndMergeScanContinuation(base, makeExtension(true));
    expect(first.ok).toBe(true);
    expect(second.ok).toBe(true);
    if (!first.ok || !second.ok) return;
    const firstIndex = pointIndexInVoxel(first.snapshot.positions, [0, 0, 0], 0.04);
    const secondIndex = pointIndexInVoxel(second.snapshot.positions, [0, 0, 0], 0.04);
    const firstTuple = [
      ...first.snapshot.positions.slice(firstIndex * 3, firstIndex * 3 + 3),
      ...first.snapshot.normals.slice(firstIndex * 3, firstIndex * 3 + 3),
    ];
    const secondTuple = [
      ...second.snapshot.positions.slice(secondIndex * 3, secondIndex * 3 + 3),
      ...second.snapshot.normals.slice(secondIndex * 3, secondIndex * 3 + 3),
    ];
    expect(firstTuple).toEqual(secondTuple);
    for (const [index, expected] of [0.01, 0.03, 0.02, 0, -1, 0].entries()) {
      expect(firstTuple[index]).toBeCloseTo(expected);
    }
  });

  it('selects the TSDF tuple with better normal readiness and preserves every field from that source', () => {
    const points = cloudPoints();
    const base = snapshot(points);
    const extension = snapshot(points);
    base.tsdfNormals.set([1, 0, 0], 0);
    base.tsdfConfidence[0] = 0.4;
    base.tsdfResidualMeters[0] = 0.12;
    base.tsdfNormalConfidence[0] = 0;
    base.tsdfCoverageConfidence[0] = 0.2;
    base.tsdfCoverageReady[0] = 0;
    extension.tsdfPositions.set([0.03, 0.02, 0.12], 0);
    extension.tsdfNormals.set([0, 0, 1], 0);
    extension.tsdfConfidence[0] = 0.96;
    extension.tsdfResidualMeters[0] = 0.01;
    extension.tsdfNormalConfidence[0] = 0.95;
    extension.tsdfCoverageConfidence[0] = 0.99;
    extension.tsdfCoverageReady[0] = 1;

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    const index = pointIndexInVoxel(result.snapshot.tsdfPositions, [0, 0, 0.1], 0.04);
    for (const [axis, expected] of [0.03, 0.02, 0.12].entries()) {
      expect(result.snapshot.tsdfPositions[index * 3 + axis]).toBeCloseTo(expected);
    }
    expect(Array.from(result.snapshot.tsdfNormals.slice(index * 3, index * 3 + 3))).toEqual([0, 0, 1]);
    expect(result.snapshot.tsdfConfidence[index]).toBeCloseTo(0.96);
    expect(result.snapshot.tsdfResidualMeters[index]).toBeCloseTo(0.01);
    expect(result.snapshot.tsdfNormalConfidence[index]).toBeCloseTo(0.95);
    expect(result.snapshot.tsdfCoverageConfidence[index]).toBeCloseTo(0.99);
    expect(result.snapshot.tsdfCoverageReady[index]).toBe(1);
  });

  it('keeps continuation annotations within total budgets and reports deterministic truncation', () => {
    const base = snapshot(cloudPoints(), {
      manualControlPoints: Array.from({ length: 250 }, (_, index) => ({ id: index + 1, position: [index, 0, 0] as const })),
      operatorEdges: Array.from({ length: 120 }, (_, index) => ({
        id: index + 1,
        start: [index, 0, 0] as const,
        end: [index + 0.1, 0, 0] as const,
        confidence: 1,
      })),
    });
    const extension = snapshot(cloudPoints(), {
      manualControlPoints: Array.from({ length: 20 }, (_, index) => ({ id: 100 - index, position: [index, 1, 0] as const })),
      operatorEdges: Array.from({ length: 20 }, (_, index) => ({
        id: 100 - index,
        start: [index, 1, 0] as const,
        end: [index + 0.1, 1, 0] as const,
        confidence: 0.9,
      })),
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.manualControlPoints).toHaveLength(256);
    expect(result.snapshot.manualControlPoints.slice(0, 250)).toEqual(base.manualControlPoints);
    expect(result.snapshot.manualControlPoints.slice(250).map((point) => point.id)).toEqual([251, 252, 253, 254, 255, 256]);
    expect(result.snapshot.operatorEdges).toHaveLength(128);
    expect(result.snapshot.operatorEdges.slice(0, 120)).toEqual(base.operatorEdges);
    expect(result.diagnostics.annotationsTruncated).toBe(true);
  });

  it('preserves distinct fine FREE/OCCUPIED cells when UNKNOWN uses a coarse grid', () => {
    const free = new Float32Array([
      0.05, 0.05, 0.05,
      0.15, 0.05, 0.05,
      0.25, 0.05, 0.05,
      0.35, 0.05, 0.05,
    ]);
    const occupied = new Float32Array([
      0.05, 0.15, 0.05,
      0.15, 0.15, 0.05,
      0.25, 0.15, 0.05,
      0.35, 0.15, 0.05,
    ]);
    const envelope = { min: [0, 0, 0] as const, max: [0.39, 0.39, 0.39] as const };
    const base = snapshot(cloudPoints(), {
      freeAirPositions: free,
      occupiedSurfacePositions: occupied,
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
      occupancyEnvelopeBounds: envelope,
      occupancyCellsSaturated: true,
      occupancyDroppedCellUpdates: 9,
      occupancyEnvelopeComponentCount: 2,
      occupancyQuarantinedSurfaceComponentCount: 1,
      occupancyQuarantinedSurfacePointCount: 4,
      occupancyBridgeRescanComponentCount: 2,
    });
    const extension = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
      occupancyEnvelopeBounds: envelope,
      occupancyDroppedCellUpdates: 3,
      occupancyEnvelopeComponentCount: 3,
      occupancyQuarantinedSurfaceComponentCount: 2,
      occupancyQuarantinedSurfacePointCount: 5,
      occupancyBridgeRescanComponentCount: 1,
      occupancyBridgeEvidenceTruncated: true,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.knownFreeVoxelCount).toBe(4);
    expect(result.snapshot.occupiedSurfaceVoxelCount).toBe(4);
    expect(result.snapshot.unknownCandidateVoxelCount).toBe(result.snapshot.unknownCandidatePositions.length / 3);
    expect(result.snapshot.occupancyFineVoxelSizeMeters).toBe(0.1);
    expect(result.snapshot.occupancyCellsSaturated).toBe(true);
    expect(result.snapshot.occupancyDroppedCellUpdates).toBe(12);
    expect(result.snapshot.occupancyEnvelopeComponentCount).toBe(5);
    expect(result.snapshot.occupancyQuarantinedSurfaceComponentCount).toBe(3);
    expect(result.snapshot.occupancyQuarantinedSurfacePointCount).toBe(9);
    expect(result.snapshot.occupancyBridgeRescanComponentCount).toBe(3);
    expect(result.snapshot.occupancyBridgeEvidenceTruncated).toBe(true);

    const decoded = decodeOccupancySolverArtifact(encodeOccupancySolverArtifact(result.snapshot, {
      frameId: 'fine-grid-continuation',
      createdAtMs: 1,
    }));
    expect(decoded.header.voxel_size_m).toBe(0.1);
    expect(sortedIndices(decoded.free)).toEqual([0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0]);
    expect(sortedIndices(decoded.occupied)).toEqual([0, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1, 0]);
  });

  it('accepts different UNKNOWN strides and regenerates them from the merged envelope policy', () => {
    const base = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.2,
      occupancyEnvelopeBounds: { min: [0, 0, 0], max: [5, 5, 5] },
    });
    const extension = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
      occupancyEnvelopeBounds: { min: [0, 0, 0], max: [15.9, 15.9, 15.9] },
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.occupancyFineVoxelSizeMeters).toBe(0.1);
    expect(result.snapshot.occupancyVoxelSizeMeters).toBeCloseTo(0.4, 6);
    expect(result.snapshot.unknownCandidateVoxelCount).toBe(result.snapshot.unknownCandidatePositions.length / 3);
    expect(result.snapshot.knownFreeVoxelCount).toBe(result.snapshot.freeAirPositions.length / 3);
    expect(result.snapshot.occupiedSurfaceVoxelCount).toBe(result.snapshot.occupiedSurfacePositions.length / 3);
  });

  it('keeps the physical envelope when boundary candidates are classified and UNKNOWN remains only at the center', () => {
    const free: number[] = [];
    const occupied: number[] = [];
    for (let x = 0; x <= 20; x += 1) {
      for (let y = 0; y <= 20; y += 1) {
        for (let z = 0; z <= 20; z += 1) {
          if (x === 10 && y === 10 && z === 10) continue;
          const destination = (x + y + z) % 2 === 0 ? free : occupied;
          destination.push((x + 0.5) * 0.1, (y + 0.5) * 0.1, (z + 0.5) * 0.1);
        }
      }
    }
    const envelope = { min: [0, 0, 0] as const, max: [2, 2, 2] as const };
    const base = snapshot(cloudPoints(), {
      freeAirPositions: new Float32Array(free),
      occupiedSurfacePositions: new Float32Array(occupied),
      unknownCandidatePositions: new Float32Array([1.05, 1.05, 1.05]),
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.1,
      occupancyEnvelopeBounds: envelope,
    });
    const extension = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.1,
      occupancyEnvelopeBounds: envelope,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.unknownCandidateVoxelCount).toBe(1);
    expect(result.snapshot.occupancyEnvelopeBounds).toEqual(envelope);
  });

  it('retains the envelope when the candidate volume is fully classified', () => {
    const free: number[] = [];
    const occupied: number[] = [];
    for (let x = 0; x <= 20; x += 1) {
      for (let y = 0; y <= 20; y += 1) {
        for (let z = 0; z <= 20; z += 1) {
          const destination = (x + y + z) % 2 === 0 ? free : occupied;
          destination.push((x + 0.5) * 0.1, (y + 0.5) * 0.1, (z + 0.5) * 0.1);
        }
      }
    }
    const envelope = { min: [0, 0, 0] as const, max: [2, 2, 2] as const };
    const base = snapshot(cloudPoints(), {
      freeAirPositions: new Float32Array(free),
      occupiedSurfacePositions: new Float32Array(occupied),
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.1,
      occupancyEnvelopeBounds: envelope,
      occupancySolverCandidateBelowSurfaceMeters: 2,
    });
    const extension = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.1,
      occupancyEnvelopeBounds: envelope,
      occupancySolverCandidateBelowSurfaceMeters: 1,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.unknownCandidatePositions).toHaveLength(0);
    expect(result.snapshot.unknownCandidateVoxelCount).toBe(0);
    expect(result.snapshot.occupancyEnvelopeBounds).toEqual(envelope);
    expect(result.snapshot.occupancySolverCandidateBelowSurfaceMeters).toBe(2);
  });

  it('transforms all eight corners of a non-square continuation envelope before union', () => {
    const world = cloudPoints();
    const yaw = Math.PI / 4;
    const translation: readonly [number, number, number] = [0.3, -0.2, 0.4];
    const baseEnvelope = { min: [0, 0, 0] as const, max: [0.1, 0.1, 0.1] as const };
    const extensionEnvelope = { min: [0, 0, 0] as const, max: [4, 1, 2] as const };
    const base = snapshot(world, {
      occupancyEnvelopeBounds: baseEnvelope,
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.3,
    });
    const extension = snapshot(transformIntoSession(world, yaw, translation), {
      occupancyEnvelopeBounds: extensionEnvelope,
      unknownCandidatePositions: new Float32Array([0.15, 0.15, 0.15, 3.75, 0.75, 1.95]),
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.3,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    const { yawRadians, translation: registeredTranslation } = result.diagnostics;
    const c = Math.cos(yawRadians), s = Math.sin(yawRadians);
    const corners: Array<[number, number, number]> = [];
    for (let corner = 0; corner < 8; corner += 1) {
      const x = (corner & 1) === 0 ? extensionEnvelope.min[0] : extensionEnvelope.max[0];
      const y = (corner & 2) === 0 ? extensionEnvelope.min[1] : extensionEnvelope.max[1];
      const z = (corner & 4) === 0 ? extensionEnvelope.min[2] : extensionEnvelope.max[2];
      corners.push([
        c * x - s * y + registeredTranslation[0],
        s * x + c * y + registeredTranslation[1],
        z + registeredTranslation[2],
      ]);
    }
    const endpointMin = corners[0];
    const endpointMax = corners[7];
    const missedCorner = corners[1];
    expect(missedCorner[0]).toBeGreaterThan(Math.max(endpointMin[0], endpointMax[0]) + 0.2);

    const expectedMin = [
      Math.min(baseEnvelope.min[0], ...corners.map((corner) => corner[0])),
      Math.min(baseEnvelope.min[1], ...corners.map((corner) => corner[1])),
      Math.min(baseEnvelope.min[2], ...corners.map((corner) => corner[2])),
    ];
    const expectedMax = [
      Math.max(baseEnvelope.max[0], ...corners.map((corner) => corner[0])),
      Math.max(baseEnvelope.max[1], ...corners.map((corner) => corner[1])),
      Math.max(baseEnvelope.max[2], ...corners.map((corner) => corner[2])),
    ];
    expect(result.snapshot.occupancyEnvelopeBounds?.min[0]).toBeCloseTo(expectedMin[0], 4);
    expect(result.snapshot.occupancyEnvelopeBounds?.min[1]).toBeCloseTo(expectedMin[1], 4);
    expect(result.snapshot.occupancyEnvelopeBounds?.min[2]).toBeCloseTo(expectedMin[2], 4);
    expect(result.snapshot.occupancyEnvelopeBounds?.max[0]).toBeCloseTo(expectedMax[0], 4);
    expect(result.snapshot.occupancyEnvelopeBounds?.max[1]).toBeCloseTo(expectedMax[1], 4);
    expect(result.snapshot.occupancyEnvelopeBounds?.max[2]).toBeCloseTo(expectedMax[2], 4);
  });

  it('clears below-surface distance when neither merged scan has an envelope', () => {
    const base = snapshot(cloudPoints(), {
      occupancyEnvelopeBounds: null,
      occupancySolverCandidateBelowSurfaceMeters: 2,
    });
    const extension = snapshot(cloudPoints(), {
      occupancyEnvelopeBounds: null,
      occupancySolverCandidateBelowSurfaceMeters: 2,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.occupancyEnvelopeBounds).toBeNull();
    expect(result.snapshot.occupancySolverCandidateBelowSurfaceMeters).toBe(0);
  });

  it('uses the defined transformed envelope and maximum below-surface distance', () => {
    const extensionEnvelope = { min: [1, 1, 1] as const, max: [2, 2, 2] as const };
    const base = snapshot(cloudPoints(), {
      occupancyEnvelopeBounds: null,
      occupancySolverCandidateBelowSurfaceMeters: 1,
    });
    const extension = snapshot(cloudPoints(), {
      occupancyEnvelopeBounds: extensionEnvelope,
      occupancySolverCandidateBelowSurfaceMeters: 2,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.snapshot.occupancyEnvelopeBounds).not.toBeNull();
    expect(result.snapshot.occupancySolverCandidateBelowSurfaceMeters).toBe(2);
  });

  it('keeps a fine FREE veto through artifact encoding where coarse deduplication loses it', () => {
    const free = new Float32Array([
      0.05, 0.05, 0.05,
      0.15, 0.05, 0.05,
      0.25, 0.05, 0.05,
      0.35, 0.05, 0.05,
    ]);
    const base = snapshot(cloudPoints(), {
      freeAirPositions: free,
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
      occupancyEnvelopeBounds: { min: [0, 0, 0], max: [0.39, 0.39, 0.39] },
    });
    const extension = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
      occupancyEnvelopeBounds: { min: [0, 0, 0], max: [0.39, 0.39, 0.39] },
    });
    // The former 0.4 m merge kept only the last of these four FREE centers.
    const legacy = snapshot(cloudPoints(), {
      freeAirPositions: free.slice(-3),
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.4,
      occupancyEnvelopeBounds: { min: [0, 0, 0], max: [0.39, 0.39, 0.39] },
    });
    const legacyDecoded = decodeOccupancySolverArtifact(encodeOccupancySolverArtifact(legacy, {
      frameId: 'legacy-coarse-dedup',
      createdAtMs: 1,
    }));
    expect(classifyDecodedOccupancy(legacyDecoded, [0.05, 0.05, 0.05])).not.toBe('free');

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    const decoded = decodeOccupancySolverArtifact(encodeOccupancySolverArtifact(result.snapshot, {
      frameId: 'free-veto-continuation',
      createdAtMs: 1,
    }));
    expect(classifyDecodedOccupancy(decoded, [0.05, 0.05, 0.05])).toBe('free');
  });

  it('rejects differing FINE occupancy sizes even when UNKNOWN sizes are otherwise compatible', () => {
    const base = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.1,
      occupancyVoxelSizeMeters: 0.2,
    });
    const extension = snapshot(cloudPoints(), {
      occupancyFineVoxelSizeMeters: 0.12,
      occupancyVoxelSizeMeters: 0.2,
    });

    const result = registerAndMergeScanContinuation(base, extension);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toContain('different occupancy fine voxel sizes');
  });

  it('requires equal surface voxel scales in either continuation direction and round-trips the chosen scale', () => {
    for (const [baseVoxelSize, extensionVoxelSize] of [[0.04, 0.02], [0.02, 0.04]] as const) {
      const base = snapshot(cloudPoints());
      const extension = snapshot(cloudPoints());
      base.voxelSizeMeters = baseVoxelSize;
      extension.voxelSizeMeters = extensionVoxelSize;
      const result = registerAndMergeScanContinuation(base, extension);
      expect(result.ok).toBe(false);
      if (!result.ok) expect(result.reason).toContain('different surface voxel sizes');
    }

    const base = snapshot(cloudPoints());
    const extension = snapshot(cloudPoints());
    base.voxelSizeMeters = extension.voxelSizeMeters = 0.06;
    const accepted = registerAndMergeScanContinuation(base, extension);
    expect(accepted.ok).toBe(true);
    if (accepted.ok) {
      expect(accepted.snapshot.voxelSizeMeters).toBe(0.06);
      expect(decodeScanArtifact(encodeScanArtifact(accepted.snapshot)).voxelSizeMeters).toBe(0.06);
    }
  });

  it('rejects a near-limit merge before allocating merged arrays and reports every family estimate', () => {
    const fixture = partialOverlapPair(1);
    setLargeTsdfEvidence(fixture.base, 110_000, 10);
    setLargeTsdfEvidence(fixture.extension, 110_000, 30);
    setOccupancyEvidence(fixture.base, 2);
    setOccupancyEvidence(fixture.extension, 8);
    const baseBytes = encodeScanArtifact(fixture.base);
    const extensionBytes = encodeScanArtifact(fixture.extension);
    expect(baseBytes.byteLength).toBeLessThan(MAX_SCAN_ARTIFACT_BYTES);
    expect(extensionBytes.byteLength).toBeLessThan(MAX_SCAN_ARTIFACT_BYTES);
    expect(baseBytes.byteLength).toBeGreaterThan(4_000_000);
    expect(extensionBytes.byteLength).toBeGreaterThan(4_000_000);

    const result = registerAndMergeScanContinuation(fixture.base, fixture.extension);
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.diagnostics?.rejectionReason).toBe('merged_budget_exceeded');
      expect(result.reason).toContain('merged artifact estimate');
      expect(result.diagnostics?.mergedBudgetEstimate?.estimatedBytes).toBeGreaterThan(MAX_SCAN_ARTIFACT_BYTES);
      expect(result.diagnostics?.mergedBudgetEstimate?.families.tsdfPositions).toBeGreaterThan(0);
      expect(result.diagnostics?.mergedBudgetEstimate?.families.tsdfCoverageReady).toBeGreaterThan(0);
      expect(result.diagnostics?.mergedBudgetEstimate?.families.freeAirPositions).toBeGreaterThan(0);
      expect(result.diagnostics?.mergedBudgetEstimate?.families.occupiedSurfacePositions).toBeGreaterThan(0);
      expect(result.diagnostics?.mergedBudgetEstimate?.families.unknownCandidatePositions).toBeGreaterThan(0);
      expect(result.diagnostics?.runtimeOperations?.scoredHypotheses).toBeLessThanOrEqual(72 * (10 + 6));
      expect(result.diagnostics?.runtimeOperations?.refinedHypotheses).toBeLessThanOrEqual(13);
      expect(result.diagnostics?.runtimeOperations?.coarseScratchBytes).toBe(2_723_840);
    }
  });
});
