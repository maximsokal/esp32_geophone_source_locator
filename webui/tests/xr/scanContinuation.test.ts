import { describe, expect, it } from 'vitest';

import {
  decodeOccupancySolverArtifact,
  encodeOccupancySolverArtifact,
} from '../../src/xr/occupancySolverArtifact';
import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
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
    expect(Math.abs(Math.abs(result.extensionToBase.yaw) - yaw)).toBeLessThan(0.05);

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
    expect(result.ok).toBe(false);
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

  it('preserves per-surfel normal confidence through reload and the same voxel ownership rule', () => {
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
    expect(result.snapshot.normalConfidence?.[0]).toBeCloseTo(0.35);
    expect(result.snapshot.normalConfidence?.[result.snapshot.pointCount - 1]).toBeCloseTo(0.97);
    const roundTrip = decodeScanArtifact(encodeScanArtifact(result.snapshot));
    expect(roundTrip.normalConfidence).toEqual(result.snapshot.normalConfidence);
  });

  it('fills unknown confidence with zero when only one side has normalConfidence', () => {
    const points = cloudPoints();
    const withConfidence = snapshot(points, { normalConfidence: new Float32Array(points.length / 3).fill(0.6) });
    const withoutConfidence = snapshot(points);
    const missingBase = registerAndMergeScanContinuation(withoutConfidence, withConfidence);
    expect(missingBase.ok).toBe(true);
    if (missingBase.ok) {
      expect(missingBase.snapshot.normalConfidence).toHaveLength(missingBase.snapshot.pointCount);
      expect(missingBase.snapshot.normalConfidence?.every((value) => value === 0)).toBe(true);
    }
    const extensionPoints = new Float32Array(points.length + 3);
    extensionPoints.set(points);
    extensionPoints.set([4, 4, 4], points.length);
    const missingExtension = registerAndMergeScanContinuation(withConfidence, snapshot(extensionPoints));
    expect(missingExtension.ok).toBe(true);
    if (missingExtension.ok) {
      expect(missingExtension.snapshot.normalConfidence).toHaveLength(missingExtension.snapshot.pointCount);
      expect(missingExtension.snapshot.normalConfidence?.slice(0, points.length / 3).every((value) => Math.abs(value - 0.6) < 1e-6)).toBe(true);
      expect(missingExtension.snapshot.normalConfidence?.[missingExtension.snapshot.pointCount - 1]).toBe(0);
    }
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
});
