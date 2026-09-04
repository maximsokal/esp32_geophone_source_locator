import { describe, expect, it } from 'vitest';

import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

function snapshot(): ScanPointCloudSnapshot {
  return {
    positions: new Float32Array([1, 2, 3, 4, 5, 6]),
    confidence: new Float32Array([0.81, 0.92]),
    normals: new Float32Array([0, 1, 0, 1, 0, 0]),
    pointCount: 2,
    tentativePositions: new Float32Array([7, 8, 9]),
    tentativeConfidence: new Float32Array([0.42]),
    tentativePointCount: 1,
    rawPointCount: 3,
    bounds: { min: [1, 2, 3], max: [4, 5, 6] },
    fullBounds: { min: [1, 2, 3], max: [7, 8, 9] },
    voxelSizeMeters: 0.04,
    maxDepthMeters: 8,
    depthFrameCount: 120,
    sampledDepthFrames: 32,
    capacityReached: false,
    maxPointCapacity: 24000,
    boundsTrimFraction: 0.02,
    coverageCellCount: 10,
    coverageCellSizeMeters: 0.12,
    adaptivePointsPerCell: 6,
    memoryBudgetPoints: 24000,
    candidateSampleCount: 700,
    discardedSampleCount: 20,
    replacementCount: 11,
    rebalanceEvictionCount: 3,
    coverageRejectedCount: 9,
    promotedPointCount: 2,
    readyCoverageCellCount: 7,
    integrationKeyframes: 19,
    polishFrames: 4,
    residualRejectedSamples: 5,
    staleTentativePruned: 6,
    averageStableResidualMeters: 0.006,
    occupancyVoxelSizeMeters: 0.18,
    knownFreeVoxelCount: 2,
    occupiedSurfaceVoxelCount: 1,
    unknownCandidateVoxelCount: 2,
    freeAirPositions: new Float32Array([0, 0, 0, 0.18, 0, 0]),
    occupiedSurfacePositions: new Float32Array([1, 2, 3]),
    unknownCandidatePositions: new Float32Array([2, 2, 2, 3, 3, 3]),
    occupancyEnvelopeBounds: { min: [0, 0, 0], max: [4, 5, 6] },
    occupancyEnvelopeComponentCount: 1,
    occupancyQuarantinedSurfaceComponentCount: 2,
    occupancyQuarantinedSurfacePointCount: 4,
    occupancyBridgeRescanComponentCount: 1,
    occupancyBridgeEvidenceTruncated: false,
    tsdfPositions: new Float32Array([1, 2, 3]),
    tsdfNormals: new Float32Array([0, 1, 0]),
    tsdfConfidence: new Float32Array([0.9]),
    tsdfResidualMeters: new Float32Array([0.004]),
    tsdfNormalConfidence: new Float32Array([0.87]),
    tsdfCoverageConfidence: new Float32Array([0.93]),
    tsdfCoverageReady: new Uint8Array([1]),
    tsdfPointCount: 1,
    manualControlPoints: [{ id: 1, position: [1, 2, 3] }],
    operatorEdges: [{ id: 1, start: [1, 2, 3], end: [4, 5, 6], confidence: 0.91 }],
  };
}

describe('3D scan artifact', () => {
  it('round-trips typed geometry and operator constraints without JSON float expansion', () => {
    const original = snapshot();
    const encoded = encodeScanArtifact(original);
    const restored = decodeScanArtifact(encoded);

    expect(restored.pointCount).toBe(original.pointCount);
    expect(Array.from(restored.positions)).toEqual(Array.from(original.positions));
    expect(Array.from(restored.normals)).toEqual(Array.from(original.normals));
    expect(Array.from(restored.freeAirPositions)).toEqual(Array.from(original.freeAirPositions));
    expect(Array.from(restored.unknownCandidatePositions)).toEqual(Array.from(original.unknownCandidatePositions));
    expect(Array.from(restored.tsdfCoverageReady)).toEqual([1]);
    expect(restored.manualControlPoints).toEqual(original.manualControlPoints);
    expect(restored.operatorEdges).toEqual(original.operatorEdges);
    expect(restored.bounds).toEqual(original.bounds);
  });

  it('rejects a truncated or modified SD artifact instead of rendering fabricated geometry', () => {
    const encoded = encodeScanArtifact(snapshot());
    const corrupted = encoded.slice();
    corrupted[corrupted.length - 1] ^= 0x01;
    expect(() => decodeScanArtifact(corrupted)).toThrow(/checksum/i);
    expect(() => decodeScanArtifact(encoded.slice(0, encoded.length - 3))).toThrow(/length/i);
  });
});
