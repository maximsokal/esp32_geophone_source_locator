import { describe, expect, it } from 'vitest';

import { solverCandidateVolume } from '../../src/xr/candidateVolume';
import { sourceToVoxelIndex, sourceVoxelGridForScan } from '../../src/xr/sourceHighlight';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

const surface = { min: [0, 0, 0] as const, max: [1, 1, 1] as const };
const paritySnapshot = {
  bounds: surface,
  occupancyEnvelopeBounds: surface,
  occupancySolverCandidateBelowSurfaceMeters: 2,
  occupancyVoxelSizeMeters: 0.5,
  occupancyFineVoxelSizeMeters: 0.5,
};

/* Kept inline so the TFT owner can mirror the exact floor and face semantics. */
type CandidateVolumeParityCase = {
  name: string;
  snapshot?: Pick<ScanPointCloudSnapshot,
    'bounds' | 'occupancyEnvelopeBounds' | 'occupancySolverCandidateBelowSurfaceMeters' |
    'occupancyVoxelSizeMeters' | 'occupancyFineVoxelSizeMeters'
  >;
  position: readonly [number, number, number];
  expected: number | null;
};

const CANDIDATE_VOLUME_PARITY_CASES: CandidateVolumeParityCase[] = [
  { name: 'exact lower face', position: [0, 0, -2] as const, expected: 0 },
  { name: 'one voxel below', position: [0.25, 0.25, -2.5] as const, expected: null },
  { name: 'upper surface', position: [0.25, 0.25, 1] as const, expected: 54 },
  { name: 'XY upper edges', position: [1, 1, -2] as const, expected: 8 },
  {
    name: 'negative floor',
    snapshot: {
      bounds: { min: [-0.6, -0.1, -0.2] as const, max: [0.6, 0.9, 0.5] as const },
      occupancyEnvelopeBounds: { min: [-0.6, -0.1, -0.2] as const, max: [0.6, 0.9, 0.5] as const },
      occupancySolverCandidateBelowSurfaceMeters: 0.4,
      occupancyVoxelSizeMeters: 0.5,
      occupancyFineVoxelSizeMeters: 0.5,
    },
    position: [-0.5, -0.25, -0.75] as const,
    expected: 1,
  },
];

describe('solver candidate volume', () => {
  it('uses component-aware envelope bounds and includes only the configured lower candidate depth', () => {
    const volume = solverCandidateVolume({
      ...paritySnapshot,
      bounds: { min: [-100, -100, -20], max: [100, 100, 20] },
    });
    expect(volume).toEqual({
      min: [0, 0, -2], max: [1, 1, 1], voxelSize: 0.5, candidateDepthUnavailable: false,
    });
  });

  it('falls back to surface bounds when envelope or depth metadata is absent', () => {
    expect(solverCandidateVolume({ bounds: surface, occupancyEnvelopeBounds: surface, occupancyVoxelSizeMeters: 0.5 }))
      .toEqual({ min: [0, 0, 0], max: [1, 1, 1], voxelSize: 0.5, candidateDepthUnavailable: true });
    expect(solverCandidateVolume({ bounds: surface, occupancySolverCandidateBelowSurfaceMeters: 2, occupancyVoxelSizeMeters: 0.5 }))
      .toMatchObject({ min: [0, 0, 0], max: [1, 1, 1], candidateDepthUnavailable: true });
  });

  it.each(CANDIDATE_VOLUME_PARITY_CASES)('matches candidate-volume mapping: $name', (testCase) => {
    const grid = sourceVoxelGridForScan(testCase.snapshot ?? paritySnapshot);
    expect(grid).not.toBeNull();
    expect(sourceToVoxelIndex(testCase.position, grid!)).toBe(testCase.expected);
  });

  it('marks missing surface bounds as unavailable without fabricating a grid', () => {
    const volume = solverCandidateVolume({ occupancyVoxelSizeMeters: 0.5 });
    expect(volume.candidateDepthUnavailable).toBe(true);
    expect(volume.min).toBeNull();
    expect(volume.max).toBeNull();
  });
});
