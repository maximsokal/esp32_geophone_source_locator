import { describe, expect, it } from 'vitest';

import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import { makeHistoricalScanArtifactFixtures } from './fixtures/scanArtifactSchema1History';
import { rewriteScanArtifactMetadata } from './support/scanArtifactTestTools';
import { makeValidScanSnapshot } from './support/validScanSnapshot';

function artifactWithMetadata(update: (metadata: Record<string, unknown>) => void): Uint8Array {
  return rewriteScanArtifactMetadata(encodeScanArtifact(makeValidScanSnapshot()), update);
}

const missingRequiredFields = [
  'manualControlPoints',
  'operatorEdges',
  'rawPointCount',
  'bounds',
  'fullBounds',
  'voxelSizeMeters',
  'maxDepthMeters',
  'depthFrameCount',
  'sampledDepthFrames',
  'capacityReached',
  'maxPointCapacity',
  'boundsTrimFraction',
  'coverageCellCount',
  'coverageCellSizeMeters',
  'adaptivePointsPerCell',
  'memoryBudgetPoints',
  'candidateSampleCount',
  'discardedSampleCount',
  'replacementCount',
  'rebalanceEvictionCount',
  'coverageRejectedCount',
  'promotedPointCount',
  'readyCoverageCellCount',
  'integrationKeyframes',
  'polishFrames',
  'residualRejectedSamples',
  'staleTentativePruned',
  'averageStableResidualMeters',
  'occupancyVoxelSizeMeters',
  'knownFreeVoxelCount',
  'occupiedSurfaceVoxelCount',
  'unknownCandidateVoxelCount',
  'occupancyEnvelopeBounds',
  'occupancyEnvelopeComponentCount',
  'occupancyQuarantinedSurfaceComponentCount',
  'occupancyQuarantinedSurfacePointCount',
  'occupancyBridgeRescanComponentCount',
  'occupancyBridgeEvidenceTruncated',
] as const;

describe('scan artifact metadata contract', () => {
  it.each(missingRequiredFields)('rejects missing required metadata field %s', (field) => {
    const encoded = artifactWithMetadata((metadata) => { delete metadata[field]; });
    expect(() => decodeScanArtifact(encoded)).toThrow(field);
  });

  it.each([
    {
      name: 'wrong boolean primitive',
      field: 'capacityReached',
      update: (metadata: Record<string, unknown>) => { metadata.capacityReached = 'false'; },
    },
    {
      name: 'negative required counter',
      field: 'depthFrameCount',
      update: (metadata: Record<string, unknown>) => { metadata.depthFrameCount = -1; },
    },
    {
      name: 'fractional required counter',
      field: 'occupancyEnvelopeComponentCount',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyEnvelopeComponentCount = 1.5; },
    },
    {
      name: 'zero occupancy voxel size',
      field: 'occupancyVoxelSizeMeters',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyVoxelSizeMeters = 0; },
    },
    {
      name: 'NaN serialized as null',
      field: 'averageStableResidualMeters',
      update: (metadata: Record<string, unknown>) => { metadata.averageStableResidualMeters = Number.NaN; },
    },
    {
      name: 'null positive size',
      field: 'maxDepthMeters',
      update: (metadata: Record<string, unknown>) => { metadata.maxDepthMeters = null; },
    },
    {
      name: 'wrong saturation primitive',
      field: 'occupancyCellsSaturated',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyCellsSaturated = 0; },
    },
    {
      name: 'fractional optional counter',
      field: 'occupancyDroppedCellUpdates',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyDroppedCellUpdates = 0.5; },
    },
    {
      name: 'negative optional counter',
      field: 'occupancyDroppedCellUpdates',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyDroppedCellUpdates = -1; },
    },
    {
      name: 'negative localization extension',
      field: 'occupancySolverCandidateBelowSurfaceMeters',
      update: (metadata: Record<string, unknown>) => { metadata.occupancySolverCandidateBelowSurfaceMeters = -0.1; },
    },
    {
      name: 'NaN optional voxel size',
      field: 'occupancyFineVoxelSizeMeters',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyFineVoxelSizeMeters = Number.NaN; },
    },
    {
      name: 'fine voxel larger than coarse voxel',
      field: 'occupancyFineVoxelSizeMeters',
      update: (metadata: Record<string, unknown>) => { metadata.occupancyFineVoxelSizeMeters = 0.24; },
    },
    {
      name: 'malformed normal confidence metadata',
      field: 'normalConfidence',
      update: (metadata: Record<string, unknown>) => { metadata.normalConfidence = { 0: 'high' }; },
    },
  ])('rejects $name at $field', ({ field, update }) => {
    const encoded = artifactWithMetadata(update);
    expect(() => decodeScanArtifact(encoded)).toThrow(field);
  });

  it.each([
    { name: 'wrong coordinate count', field: 'bounds', min: [0, 0], max: [1, 1, 1] },
    { name: 'non-numeric coordinate', field: 'fullBounds', min: [0, 'near', 0], max: [1, 1, 1] },
    { name: 'minimum greater than maximum', field: 'occupancyEnvelopeBounds', min: [2, 0, 0], max: [1, 1, 1] },
  ])('rejects bounds with $name in $field', ({ field, min, max }) => {
    const encoded = artifactWithMetadata((metadata) => { metadata[field] = { min, max }; });
    expect(() => decodeScanArtifact(encoded)).toThrow(field);
  });

  it.each([
    { field: 'knownFreeVoxelCount', value: 2 },
    { field: 'occupiedSurfaceVoxelCount', value: 2 },
    { field: 'unknownCandidateVoxelCount', value: 2 },
  ])('rejects $field that disagrees with its persisted point array', ({ field, value }) => {
    const encoded = artifactWithMetadata((metadata) => { metadata[field] = value; });
    expect(() => decodeScanArtifact(encoded)).toThrow(field);
  });

  it.each([
    {
      name: 'non-finite coordinate',
      update: (metadata: Record<string, unknown>) => {
        metadata.manualControlPoints = [{ id: 1, position: [0, Number.NaN, 0] }];
      },
      field: 'manualControlPoints[0].position[1]',
    },
    {
      name: 'duplicate point id',
      update: (metadata: Record<string, unknown>) => {
        metadata.manualControlPoints = [
          { id: 3, position: [0, 0, 0] },
          { id: 3, position: [1, 1, 1] },
        ];
      },
      field: 'manualControlPoints[1].id',
    },
  ])('rejects manual control points with $name', ({ update, field }) => {
    const encoded = artifactWithMetadata(update);
    expect(() => decodeScanArtifact(encoded)).toThrow(field);
  });

  it.each([
    {
      name: 'edge shorter than the runtime minimum',
      update: (metadata: Record<string, unknown>) => {
        metadata.operatorEdges = [{ id: 1, start: [0, 0, 0], end: [0.02, 0, 0], confidence: 0.9 }];
      },
      field: 'operatorEdges[0]',
    },
    {
      name: 'out-of-range confidence',
      update: (metadata: Record<string, unknown>) => {
        metadata.operatorEdges = [{ id: 1, start: [0, 0, 0], end: [0.1, 0, 0], confidence: 1.2 }];
      },
      field: 'operatorEdges[0].confidence',
    },
    {
      name: 'duplicate edge id',
      update: (metadata: Record<string, unknown>) => {
        metadata.operatorEdges = [
          { id: 2, start: [0, 0, 0], end: [0.1, 0, 0], confidence: 0.9 },
          { id: 2, start: [1, 0, 0], end: [1.1, 0, 0], confidence: 0.9 },
        ];
      },
      field: 'operatorEdges[1].id',
    },
  ])('rejects operator edges with $name', ({ update, field }) => {
    const encoded = artifactWithMetadata(update);
    expect(() => decodeScanArtifact(encoded)).toThrow(field);
  });

  it('drops unknown additive metadata keys instead of returning them in the snapshot', () => {
    const encoded = artifactWithMetadata((metadata) => { metadata.futureDiagnostic = { revision: 9 }; });
    const decoded = decodeScanArtifact(encoded);

    expect(Object.hasOwn(decoded, 'futureDiagnostic')).toBe(false);
  });

  it('keeps absent optional fields absent without inventing localization evidence', () => {
    const encoded = rewriteScanArtifactMetadata(encodeScanArtifact(makeValidScanSnapshot()), (metadata) => {
      delete metadata.normalConfidence;
      delete metadata.occupancyFineVoxelSizeMeters;
      delete metadata.occupancyCellsSaturated;
      delete metadata.occupancyDroppedCellUpdates;
      delete metadata.occupancySolverCandidateBelowSurfaceMeters;
    });
    const decoded = decodeScanArtifact(encoded);

    expect(Object.hasOwn(decoded, 'normalConfidence')).toBe(false);
    expect(Object.hasOwn(decoded, 'occupancyFineVoxelSizeMeters')).toBe(false);
    expect(Object.hasOwn(decoded, 'occupancyCellsSaturated')).toBe(false);
    expect(Object.hasOwn(decoded, 'occupancyDroppedCellUpdates')).toBe(false);
    expect(Object.hasOwn(decoded, 'occupancySolverCandidateBelowSurfaceMeters')).toBe(false);
  });

  it.each(makeHistoricalScanArtifactFixtures())('supports the historical schema-1 shape: $name', (fixture) => {
    const decoded = decodeScanArtifact(fixture.bytes);
    for (const field of fixture.absentFields) expect(Object.hasOwn(decoded, field), field).toBe(false);
    for (const field of fixture.presentFields) expect(Object.hasOwn(decoded, field), field).toBe(true);
    if (fixture.presentFields.includes('normalConfidence')) {
      expect(decoded.normalConfidence).toEqual(new Float32Array([0.88]));
    }
  });

  it('refuses to encode an incomplete snapshot before producing bytes', () => {
    const incomplete = makeValidScanSnapshot();
    Reflect.deleteProperty(incomplete, 'operatorEdges');

    expect(() => encodeScanArtifact(incomplete)).toThrow('operatorEdges');
  });
});
