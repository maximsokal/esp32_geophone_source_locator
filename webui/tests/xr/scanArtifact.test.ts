import { describe, expect, it } from 'vitest';

import {
  decodeScanArtifact,
  decodeScanArtifactWithIdentity,
  encodeScanArtifact,
  encodeScanArtifactWithIdentity,
  getScanArtifactMetrics,
  resetScanArtifactMetrics,
  SCAN_ARTIFACT_BINARY_ARRAY_KEYS,
} from '../../src/xr/scanArtifact';
import { getScanIdentityMetrics, resetScanIdentityMetrics } from '../../src/xr/scanIdentity';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { rewriteScanArtifactMetadata } from './support/scanArtifactTestTools';

function snapshot(): ScanPointCloudSnapshot {
  return {
    positions: new Float32Array([1, 2, 3, 4, 5, 6]),
    confidence: new Float32Array([0.81, 0.92]),
    normals: new Float32Array([0, 1, 0, 1, 0, 0]),
    normalConfidence: new Float32Array([0.95, 0.9]),
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
    occupancyCellsSaturated: false,
    occupancyFineVoxelSizeMeters: 0.06,
    occupancyDroppedCellUpdates: 0,
    occupancyEnvelopeBounds: { min: [0, 0, 0], max: [4, 5, 6] },
    occupancySolverCandidateBelowSurfaceMeters: 2,
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

    expect(restored).toEqual(original);
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

  it('stores normalConfidence in the binary table and restores it as a bounded Float32Array', () => {
    const encoded = encodeScanArtifact(snapshot());
    const headerLength = new DataView(encoded.buffer).getUint32(8, true);
    const header = JSON.parse(new TextDecoder().decode(encoded.subarray(20, 20 + headerLength)));
    const restored = decodeScanArtifact(encoded);

    expect(header.metadata).not.toHaveProperty('normalConfidence');
    expect(header.lengths.normalConfidence).toBe(snapshot().pointCount);
    expect(restored.normalConfidence).toBeInstanceOf(Float32Array);
    expect(restored.normalConfidence).toEqual(new Float32Array([0.95, 0.9]));
  });

  it('keeps the schema-1 payload table unchanged when normalConfidence is absent', () => {
    const withoutNormalConfidence = { ...snapshot() };
    delete withoutNormalConfidence.normalConfidence;
    const encoded = encodeScanArtifact(withoutNormalConfidence);
    const headerLength = new DataView(encoded.buffer).getUint32(8, true);
    const header = JSON.parse(new TextDecoder().decode(encoded.subarray(20, 20 + headerLength)));

    expect(header.metadata).not.toHaveProperty('normalConfidence');
    expect(header.lengths).not.toHaveProperty('normalConfidence');
    expect(decodeScanArtifact(encoded)).not.toHaveProperty('normalConfidence');
  });

  it('keeps normalConfidence metadata bounded for a 50,000-point scan', () => {
    const large = snapshot();
    large.pointCount = 50000;
    large.maxPointCapacity = 50000;
    large.memoryBudgetPoints = 50000;
    large.positions = new Float32Array(large.pointCount * 3);
    large.confidence = new Float32Array(large.pointCount).fill(0.8);
    large.normals = new Float32Array(large.pointCount * 3);
    large.normalConfidence = new Float32Array(large.pointCount).fill(0.75);
    const encoded = encodeScanArtifact(large);
    const headerLength = new DataView(encoded.buffer).getUint32(8, true);

    expect(headerLength).toBeLessThan(4096);
    expect(decodeScanArtifact(encoded).normalConfidence).toHaveLength(large.pointCount);
  });

  it('classifies every typed-array snapshot field as binary persisted', () => {
    const typedArrayFields = Object.entries(snapshot())
      .filter(([, value]) => ArrayBuffer.isView(value))
      .map(([field]) => field);
    const binaryFields: readonly string[] = SCAN_ARTIFACT_BINARY_ARRAY_KEYS;

    expect(typedArrayFields.length).toBeGreaterThan(0);
    expect(typedArrayFields.every((field) => binaryFields.includes(field))).toBe(true);
  });

  it('decodes binary arrays as views over one owned payload copy', () => {
    const decoded = decodeScanArtifact(encodeScanArtifact(snapshot()));

    expect(decoded.positions.buffer).toBe(decoded.confidence.buffer);
    expect(decoded.normalConfidence?.buffer).toBe(decoded.positions.buffer);
    expect(decoded.tsdfCoverageReady.buffer).toBe(decoded.positions.buffer);
  });

  it('measures validation and checksum passes while producing one scan identity', () => {
    resetScanArtifactMetrics();
    resetScanIdentityMetrics();
    const encoded = encodeScanArtifactWithIdentity(snapshot());
    const headerLength = new DataView(encoded.bytes.buffer).getUint32(8, true);
    const payloadLength = new DataView(encoded.bytes.buffer).getUint32(12, true);

    expect(getScanArtifactMetrics().validationBytesVisited).toBeGreaterThan(0);
    expect(getScanArtifactMetrics().artifactChecksumCrcBytesVisited).toBe(headerLength + payloadLength);
    expect(getScanIdentityMetrics()).toMatchObject({
      wholeScanCrcInvocations: 1,
      wholeScanCrcBytesVisited: encoded.bytes.byteLength,
    });

    resetScanArtifactMetrics();
    resetScanIdentityMetrics();
    const decoded = decodeScanArtifactWithIdentity(encoded.bytes);
    expect(decoded.identity).toEqual(encoded.identity);
    expect(getScanArtifactMetrics().artifactChecksumCrcBytesVisited).toBe(encoded.bytes.byteLength - 20);
    expect(getScanIdentityMetrics()).toMatchObject({
      wholeScanCrcInvocations: 1,
      wholeScanCrcBytesVisited: encoded.bytes.byteLength,
    });
  });

  it('rejects a truncated or modified SD artifact instead of rendering fabricated geometry', () => {
    const encoded = encodeScanArtifact(snapshot());
    const corrupted = encoded.slice();
    corrupted[corrupted.length - 1] ^= 0x01;
    expect(() => decodeScanArtifact(corrupted)).toThrow(/checksum/i);
    expect(() => decodeScanArtifact(encoded.slice(0, encoded.length - 3))).toThrow(/checksum/i);
  });

  it('decodes the original round-trip shape with later schema-1 additions absent', () => {
    const original = snapshot();
    // Older schema-1 artifacts have no binary normalConfidence length entry.
    delete original.normalConfidence;
    const legacyExpected = { ...original };
    const optionalFields = [
      'normalConfidence',
      'occupancyFineVoxelSizeMeters',
      'occupancyCellsSaturated',
      'occupancyDroppedCellUpdates',
      'occupancySolverCandidateBelowSurfaceMeters',
    ];
    for (const field of optionalFields) Reflect.deleteProperty(legacyExpected, field);

    const encoded = rewriteScanArtifactMetadata(encodeScanArtifact(original), (metadata) => {
      for (const field of optionalFields) delete metadata[field];
    });
    expect(decodeScanArtifact(encoded)).toEqual(legacyExpected);
  });
});
