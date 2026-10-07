import { encodeScanArtifact } from '../../../src/xr/scanArtifact';
import { makeValidScanSnapshot } from '../support/validScanSnapshot';
import { rewriteNormalConfidenceAsLegacy, rewriteScanArtifactMetadata } from '../support/scanArtifactTestTools';

export interface HistoricalScanArtifactFixture {
  name: string;
  absentFields: readonly string[];
  presentFields: readonly string[];
  bytes: Uint8Array;
}

const LATER_FIELDS = [
  'normalConfidence',
  'occupancyCellsSaturated',
  'occupancyDroppedCellUpdates',
  'occupancySolverCandidateBelowSurfaceMeters',
  'occupancyFineVoxelSizeMeters',
] as const;

export function makeHistoricalScanArtifactFixtures(): HistoricalScanArtifactFixture[] {
  const current = encodeScanArtifact(makeValidScanSnapshot());
  const withoutNormalConfidence = rewriteNormalConfidenceAsLegacy(current, false);
  const withLegacyNormalConfidence = rewriteNormalConfidenceAsLegacy(current, true);
  const stages = [
    {
      name: 'first schema-1 emission (0d66a158)',
      source: withoutNormalConfidence,
      absentFields: LATER_FIELDS,
      presentFields: [] as const,
    },
    {
      name: 'fine occupancy voxel addition (a7bed6a5)',
      source: withoutNormalConfidence,
      absentFields: [
        'normalConfidence',
        'occupancyCellsSaturated',
        'occupancyDroppedCellUpdates',
        'occupancySolverCandidateBelowSurfaceMeters',
      ] as const,
      presentFields: ['occupancyFineVoxelSizeMeters'] as const,
    },
    {
      name: 'occupancy diagnostics addition (c1db8590)',
      source: withoutNormalConfidence,
      absentFields: ['normalConfidence'] as const,
      presentFields: [
        'occupancyFineVoxelSizeMeters',
        'occupancyCellsSaturated',
        'occupancyDroppedCellUpdates',
        'occupancySolverCandidateBelowSurfaceMeters',
      ] as const,
    },
    {
      name: 'normal confidence addition (74867942)',
      source: withLegacyNormalConfidence,
      absentFields: [] as const,
      presentFields: LATER_FIELDS,
    },
  ];

  return stages.map((stage) => ({
    ...stage,
    bytes: rewriteScanArtifactMetadata(stage.source, (metadata) => {
      for (const field of stage.absentFields) delete metadata[field];
    }),
  }));
}
