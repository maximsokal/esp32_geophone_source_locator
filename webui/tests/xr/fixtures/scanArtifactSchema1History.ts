import { encodeScanArtifact } from '../../../src/xr/scanArtifact';
import { makeValidScanSnapshot } from '../support/validScanSnapshot';
import { rewriteScanArtifactMetadata } from '../support/scanArtifactTestTools';

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
  const stages = [
    {
      name: 'first schema-1 emission (0d66a158)',
      absentFields: LATER_FIELDS,
      presentFields: [] as const,
    },
    {
      name: 'fine occupancy voxel addition (a7bed6a5)',
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
      absentFields: [] as const,
      presentFields: LATER_FIELDS,
    },
  ];

  return stages.map((stage) => ({
    ...stage,
    bytes: rewriteScanArtifactMetadata(current, (metadata) => {
      for (const field of stage.absentFields) delete metadata[field];
    }),
  }));
}
