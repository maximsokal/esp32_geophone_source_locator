import { describe, expect, it } from 'vitest';
import candidateVolumeVectors from '../../../tests/fixtures/candidate_volume_vectors.json';

import { solverCandidateVolume } from '../../src/xr/candidateVolume';
import {
  buildSourceHighlightScene,
  sourceToVoxelIndex,
  sourceVoxelGridForScan,
} from '../../src/xr/sourceHighlight';
import type { LocalizationRun } from '../../src/api/deviceApi';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

const surface = { min: [0, 0, 0] as const, max: [1, 1, 1] as const };
const paritySnapshot = {
  bounds: surface,
  occupancyEnvelopeBounds: surface,
  occupancySolverCandidateBelowSurfaceMeters: 2,
  occupancyVoxelSizeMeters: 0.5,
  occupancyFineVoxelSizeMeters: 0.5,
};

const SHARED_VECTOR_IDENTITY = { objectId: 'OBJ_0001', sessionId: 'S000001' } as const;
const SENSOR_TO_LOCAL_FLOOR = [1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1];
const SHARED_SCAN_ID = 'scan-crc32:current';

function registration(): RegistrationArtifact {
  return {
    schema: 1, source: 'apriltag', family: 'tag36h11', geometry_computed_at_ms: 1, created_at_ms: 1,
    scan_frame_id: SHARED_SCAN_ID,
    tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
    bindings: [], sensor_to_scan: SENSOR_TO_LOCAL_FLOOR, mirrored: false, residual_rms_m: 0, threshold_m: 0.05,
    per_node: [], tier: 'minimum', valid: true, reason: null,
  };
}

function localizationRun(position: readonly [number, number, number]): LocalizationRun {
  return {
    schema_version: 4, object_id: SHARED_VECTOR_IDENTITY.objectId, session_id: SHARED_VECTOR_IDENTITY.sessionId,
    computed_at_ms: 1, kind: 'individual_method', method_name: 'method_a', method_version: 1,
    channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'capable_3d', geometry_computed_at_ms: 1,
    propagation_model_name: '', propagation_speed_mps: 340, propagation_speed_uncertainty_mps: 0,
    confidence_score: 0.5, solved: true,
    source_position: { x_m: position[0], y_m: position[1], z_m: position[2] },
  };
}

const SHARED_CANDIDATE_CASES = candidateVolumeVectors.vectors.map((row, index) => ({
  name: candidateVolumeVectors.names[index],
  envelopeMin: [row[0], row[1], row[2]] as const,
  envelopeMax: [row[3], row[4], row[5]] as const,
  belowSurfaceMeters: row[6],
  voxelSizeMeters: row[7],
  position: [row[8], row[9], row[10]] as const,
  inside: row[11] === 1,
}));

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

  it.each(SHARED_CANDIDATE_CASES)('matches shared candidate-volume vector: $name', (testCase) => {
    const bounds = { min: testCase.envelopeMin, max: testCase.envelopeMax };
    const snapshot = {
      bounds,
      occupancyEnvelopeBounds: bounds,
      occupancySolverCandidateBelowSurfaceMeters: testCase.belowSurfaceMeters,
      occupancyVoxelSizeMeters: testCase.voxelSizeMeters,
      occupancyFineVoxelSizeMeters: testCase.voxelSizeMeters,
    };
    const grid = sourceVoxelGridForScan(snapshot);
    expect(grid).not.toBeNull();
    expect(sourceToVoxelIndex(testCase.position, grid!) !== null).toBe(testCase.inside);

    const scene = buildSourceHighlightScene(
      [localizationRun(testCase.position)],
      SHARED_VECTOR_IDENTITY,
      registration(),
      grid,
      { currentScanId: SHARED_SCAN_ID },
    );
    expect(scene.primary).not.toBeNull();
    expect(scene.primary?.voxel !== null).toBe(testCase.inside);
    expect(scene.reason).toBe(testCase.inside ? null : 'outside_grid');
  });

  it('marks missing surface bounds as unavailable without fabricating a grid', () => {
    const volume = solverCandidateVolume({ occupancyVoxelSizeMeters: 0.5 });
    expect(volume.candidateDepthUnavailable).toBe(true);
    expect(volume.min).toBeNull();
    expect(volume.max).toBeNull();
  });
});
