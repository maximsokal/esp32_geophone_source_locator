import { describe, expect, it } from 'vitest';

import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import { registerAndMergeScanContinuation } from '../../src/xr/scanContinuation';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

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
  occupancy: Partial<Pick<ScanPointCloudSnapshot,
    'freeAirPositions' | 'occupiedSurfacePositions' | 'unknownCandidatePositions'>> = {},
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
    occupancyVoxelSizeMeters: 0.18,
    knownFreeVoxelCount: freeAirPositions.length / 3,
    occupiedSurfaceVoxelCount: occupiedSurfacePositions.length / 3,
    unknownCandidateVoxelCount: unknownCandidatePositions.length / 3,
    freeAirPositions,
    occupiedSurfacePositions,
    unknownCandidatePositions,
    occupancyEnvelopeBounds: boundsFor(positions),
    occupancyEnvelopeComponentCount: 1,
    occupancyQuarantinedSurfaceComponentCount: 0,
    occupancyQuarantinedSurfacePointCount: 0,
    occupancyBridgeRescanComponentCount: 0,
    occupancyBridgeEvidenceTruncated: false,
    tsdfPositions: positions.slice(),
    tsdfNormals: normals.slice(),
    tsdfConfidence: confidence.slice(),
    tsdfResidualMeters: new Float32Array(confidence.length).fill(0.012),
    tsdfNormalConfidence: new Float32Array(confidence.length).fill(0.9),
    tsdfCoverageConfidence: new Float32Array(confidence.length).fill(0.9),
    tsdfCoverageReady: new Uint8Array(confidence.length).fill(1),
    tsdfPointCount: confidence.length,
    manualControlPoints: [],
    operatorEdges: [],
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
});
