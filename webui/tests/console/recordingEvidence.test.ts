import { describe, expect, it } from 'vitest';

import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import { UNDETERMINED_UNCERTAINTY_MM, buildRecordingEvidence } from '../../src/domain/recordingEvidence';
import { defaultRegistrationTagSizes, type RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';

/* MAX-169: per-recording evidence from that recording's own frozen data. */

function run(method: string, solved: boolean, at = 10): LocalizationRun {
  return {
    schema_version: 1, object_id: 'o', session_id: 's', computed_at_ms: at, kind: 'individual_method',
    method_name: method, method_version: 1, channel_mask_used: 15, channel_mask_rejected: 0,
    dimensionality: 'planar_2d', geometry_computed_at_ms: 5, propagation_model_name: 'c',
    propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 10, confidence_score: 0.5,
    solved, source_position: solved ? { x_m: 1, y_m: 1, z_m: 0 } : undefined,
  };
}

function listing(pods: LocalizationRunListing['geometry']['pods'], runs: LocalizationRun[] = []): LocalizationRunListing {
  return {
    object_id: 'o', session_id: 's',
    geometry: { available: true, pods },
    cs_pairs: { available: false, pairs: [] },
    picks: [],
    runs,
  };
}

const pods = [
  { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0, computed_at_ms: 900 },
  { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 100, residual_mm: 40, uncertainty_mm: 60, computed_at_ms: 900 },
  { pod: 2, computed: true, leveled: false, x_mm: 0, y_mm: 3000, z_mm: -50, residual_mm: 120, uncertainty_mm: UNDETERMINED_UNCERTAINTY_MM, computed_at_ms: 900 },
  { pod: 3, computed: true, leveled: true, x_mm: 3000, y_mm: 3000, z_mm: 0, residual_mm: 10, uncertainty_mm: 90, computed_at_ms: 950 },
  { pod: 4, computed: false, leveled: false },
];

function registration(valid: boolean): RegistrationArtifact {
  return {
    schema: 1, source: 'apriltag', family: 'tag36h11', geometry_computed_at_ms: 900, created_at_ms: 1000,
    tag_size_m: defaultRegistrationTagSizes(), bindings: [],
    sensor_to_scan: valid ? [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] : null,
    mirrored: false, residual_rms_m: 0, threshold_m: 0.05, per_node: [],
    tier: valid ? 'minimum' : null, valid, reason: valid ? null : 'missing_main',
  };
}

describe('recording evidence', () => {
  it('summarises the frozen geometry: participants, levelling, policy, quality', () => {
    const e = buildRecordingEvidence(listing(pods), { present: true, bytes: 2048 }, { present: false, bytes: 0 });
    expect(e.remotes).toEqual([1, 2, 3]);
    expect(e.remoteSummary).toBe('3/4');
    expect(e.leveledRemotes).toBe(2);
    expect(e.policy).toBe('planar_2d');
    expect(e.worstResidualM).toBeCloseTo(0.12);
    /* The undetermined sentinel is reported separately, never as 2147 km. */
    expect(e.worstUncertaintyM).toBeCloseTo(0.09);
    expect(e.undeterminedPods).toEqual([2]);
    expect(e.geometryComputedAtMs).toBe(950);
    expect(e.positions.map((p) => p.label)).toEqual(['MAIN', 'Pod 1', 'Pod 2', 'Pod 3']);
    expect(e.positions[1].position).toEqual({ x_m: 4, y_m: 0, z_m: 0.1 });
    expect(e.scan).toBe('available');
    expect(e.scanBytes).toBe(2048);
    expect(e.occupancy).toBe('absent');
    expect(e.scanRegistered).toBe(false);
  });

  it('classifies localization per method (latest run of each)', () => {
    expect(buildRecordingEvidence(listing(pods), null, null).localization).toBe('not_calculated');
    const calculated = buildRecordingEvidence(listing(pods, [run('a', false, 1), run('a', true, 2), run('b', false, 3)]), null, null);
    expect(calculated.localization).toBe('calculated');
    expect(calculated.solvedMethods).toEqual(['a']);
    expect(calculated.failedMethods).toEqual(['b']);
    expect(calculated.lastCalculatedAtMs).toBe(3);
    expect(buildRecordingEvidence(listing(pods, [run('a', false)]), null, null).localization).toBe('failed');
  });

  it('derives scanRegistered from a valid artifact and retains invalid registration reasons', () => {
    expect(buildRecordingEvidence(null, null, null, registration(true))).toMatchObject({ scanRegistered: true, registrationTier: 'minimum', registrationReason: null });
    expect(buildRecordingEvidence(null, null, null, registration(false))).toMatchObject({ scanRegistered: false, registrationTier: null, registrationReason: 'missing_main' });
    expect(buildRecordingEvidence(null, null, null).scanRegistered).toBe(false);
  });

  it('derives the policy from the number of computed REMOTEs', () => {
    const four = pods.map((p) => (p.pod === 4 ? { ...p, computed: true, x_mm: 1, y_mm: 1, z_mm: 0 } : p));
    expect(buildRecordingEvidence(listing(four), null, null).policy).toBe('capable_3d');
    const two = pods.filter((p) => p.pod <= 2);
    expect(buildRecordingEvidence(listing(two), null, null).policy).toBe('insufficient');
  });

  it('reports unknown rather than guessing when data is missing', () => {
    const e = buildRecordingEvidence(null, null, null);
    expect(e.remoteSummary).toBe('unknown');
    expect(e.policy).toBe('unknown');
    expect(e.localization).toBe('unknown');
    expect(e.scan).toBe('unknown');
    expect(e.worstResidualM).toBeNull();
    const noGeometry = buildRecordingEvidence({ ...listing([]), geometry: { available: false, pods: [] } }, null, null);
    expect(noGeometry.geometryAvailable).toBe(false);
    expect(noGeometry.remoteSummary).toBe('unknown');
  });
});
