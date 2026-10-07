import { describe, expect, it } from 'vitest';

import { defaultRegistrationTagSizes, type RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import { registrationApplicability, registrationBindingApplicability } from '../../src/xr/registrationApplicability';

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const SCAN = 'scan-crc32:0123abcd';

function artifact(patch: Partial<RegistrationArtifact> = {}): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: SCAN, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: 500, created_at_ms: 600, tag_size_m: defaultRegistrationTagSizes(), bindings: [],
    sensor_to_scan: IDENTITY, mirrored: false, residual_rms_m: 0.01, threshold_m: 0.05, per_node: [],
    tier: 'minimum', valid: true, reason: null, ...patch,
  };
}

const EXPECTED = { geometryComputedAtMs: 500, scanFrameId: SCAN };

describe('registration applicability predicate (MAX-405)', () => {
  it('accepts a registration bound to the same geometry generation and scan frame', () => {
    expect(registrationApplicability(artifact(), EXPECTED)).toEqual({ ok: true });
  });

  it('rejects each way a registration can belong to another frame, with a precise reason', () => {
    expect(registrationApplicability(null, EXPECTED)).toEqual({ ok: false, reason: 'missing' });
    expect(registrationApplicability(artifact({ valid: false, sensor_to_scan: null, tier: null, reason: 'missing_main' }), EXPECTED)).toEqual({ ok: false, reason: 'invalid' });
    expect(registrationApplicability(artifact({ sensor_to_scan: [...IDENTITY.slice(0, 15), Number.NaN] }), EXPECTED)).toEqual({ ok: false, reason: 'invalid' });
    expect(registrationApplicability(artifact({ residual_rms_m: 0.3 }), EXPECTED)).toEqual({ ok: false, reason: 'quality' });
    expect(registrationApplicability(artifact({ threshold_m: 0 }), EXPECTED)).toEqual({ ok: false, reason: 'quality' });
    expect(registrationApplicability(artifact(), { ...EXPECTED, geometryComputedAtMs: 501 })).toEqual({ ok: false, reason: 'geometry_mismatch' });
    expect(registrationApplicability(artifact({ scan_frame_id: null }), EXPECTED)).toEqual({ ok: false, reason: 'scan_unbound' });
    expect(registrationApplicability(artifact({ scan_frame_id: undefined }), EXPECTED)).toEqual({ ok: false, reason: 'scan_unbound' });
    expect(registrationApplicability(artifact(), { ...EXPECTED, scanFrameId: null })).toEqual({ ok: false, reason: 'scan_unknown' });
    expect(registrationApplicability(artifact(), { ...EXPECTED, scanFrameId: 'scan-crc32:ffffffff' })).toEqual({ ok: false, reason: 'scan_mismatch' });
  });

  it('keeps boundary equality of residual and threshold valid and skips the geometry check when it is unknown', () => {
    expect(registrationApplicability(artifact({ residual_rms_m: 0.05 }), EXPECTED)).toEqual({ ok: true });
    expect(registrationApplicability(artifact(), { geometryComputedAtMs: null, scanFrameId: SCAN })).toEqual({ ok: true });
  });

  it('binding check ignores validity so tag-only evidence is still frame-checked', () => {
    const invalid = artifact({ valid: false, sensor_to_scan: null, tier: null, reason: 'missing_main' });
    expect(registrationBindingApplicability(invalid, EXPECTED)).toEqual({ ok: true });
    expect(registrationBindingApplicability(artifact({ scan_frame_id: 'scan-crc32:ffffffff' }), EXPECTED)).toEqual({ ok: false, reason: 'scan_mismatch' });
  });
});
