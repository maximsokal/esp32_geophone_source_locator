import { describe, expect, it } from 'vitest';
import {
  decodeRegistrationArtifact,
  DEFAULT_MAIN_TAG_SIZE_M,
  DEFAULT_REMOTE_TAG_SIZE_M,
  defaultRegistrationTagSizes,
  encodeRegistrationArtifact,
  toOccupancyRegistration,
  type RegistrationArtifact,
} from '../../src/xr/fiducial/registrationArtifact';

function artifact(): RegistrationArtifact {
  return {
    schema: 1,
    frame: 'webxr_local_floor',
    scan_frame_id: null,
    source: 'apriltag',
    family: 'tag36h11',
    geometry_computed_at_ms: 1234,
    created_at_ms: 5678,
    tag_size_m: defaultRegistrationTagSizes(),
    bindings: [0, 1, 2, 3].map((tag_id) => ({
      tag_id,
      node: ['MAIN', 'G1', 'G2', 'G3'][tag_id] as 'MAIN' | 'G1' | 'G2' | 'G3',
      observation_count: 6,
      inlier_count: 5,
      position_scan_m: [tag_id * 0.1, 1, -0.2],
      spread_m: 0.002,
    })),
    sensor_to_scan: [1, 0, 0, 0.2, 0, 1, 0, -0.3, 0, 0, 1, 0.1, 0, 0, 0, 1],
    mirrored: false,
    residual_rms_m: 0.01,
    threshold_m: 0.05,
    per_node: [0, 1, 2, 3].map((tagId) => ({ node: ['MAIN', 'G1', 'G2', 'G3'][tagId] as 'MAIN' | 'G1' | 'G2' | 'G3', tagId, residualM: 0.005, status: 'anchored' as const })),
    tier: 'minimum',
    valid: true,
    reason: null,
  };
}

function encodeRaw(value: RegistrationArtifact): string { return JSON.stringify(value); }

describe('registration-v1.json artifact', () => {
  it('round-trips strict JSON fields and exports documented configurable defaults', () => {
    expect(DEFAULT_MAIN_TAG_SIZE_M).toBe(0.032);
    expect(DEFAULT_REMOTE_TAG_SIZE_M).toBe(0.032);
    expect(defaultRegistrationTagSizes(0.04, 0.03)).toEqual({ '0': 0.04, '1': 0.03, '2': 0.03, '3': 0.03, '4': 0.03 });
    const value = artifact();
    expect(decodeRegistrationArtifact(encodeRegistrationArtifact(value))).toEqual(value);
    expect(JSON.parse(encodeRegistrationArtifact(value)).frame).toBe('webxr_local_floor');

    const legacy = { ...value } as Record<string, unknown>;
    delete legacy.frame;
    expect(decodeRegistrationArtifact(JSON.stringify(legacy)).frame).toBe('webxr_local_floor');
    expect(JSON.parse(encodeRegistrationArtifact(legacy as unknown as RegistrationArtifact)).frame).toBe('webxr_local_floor');
  });

  it('rejects each unsupported schema, type, number, matrix, timestamp, and tag binding', () => {
    const base = artifact();
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, schema: 2 } as RegistrationArtifact))).toThrow(/schema/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, family: 'tag25h9' } as RegistrationArtifact))).toThrow(/family/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, source: 'aruco' } as RegistrationArtifact))).toThrow(/source/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, frame: 'unknown' } as unknown as RegistrationArtifact))).toThrow(/frame marker/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, sensor_to_scan: Array(15).fill(0) } as RegistrationArtifact))).toThrow(/16-value/);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, valid: true, sensor_to_scan: null }))).toThrow(/valid artifact/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, bindings: [{ ...base.bindings[0], tag_id: 5 }] }))).toThrow(/tag id/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, bindings: [base.bindings[0], base.bindings[0]] }))).toThrow(/duplicate/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, geometry_computed_at_ms: 0 }))).toThrow(/greater than zero/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...base, created_at_ms: Number.NaN }))).toThrow(/created_at_ms/i);
    const overflowing = encodeRegistrationArtifact(base).replace('"residual_rms_m":0.01', '"residual_rms_m":1e999');
    expect(() => decodeRegistrationArtifact(overflowing)).toThrow(/finite/i);
  });

  it('returns null for invalid artifacts and exactly adapts valid ones to the occupancy contract', () => {
    const invalid = { ...artifact(), sensor_to_scan: null, tier: null, valid: false, reason: 'missing_main', per_node: artifact().per_node.map((item) => ({ ...item, status: 'visual_only' as const })) };
    expect(toOccupancyRegistration(invalid)).toBeNull();
    const value = artifact();
    expect(toOccupancyRegistration(value)).toEqual({
      source: 'apriltag',
      geometry_computed_at_ms: 1234,
      sensor_to_scan: [1, 0, 0, 0.2, 0, 0, -1, -0.1, 0, 1, 0, -0.3, 0, 0, 0, 1],
      frame: 'project_z_up',
      residual_m: 0.01,
      threshold_m: 0.05,
    });
  });

  it('enforces the quality invariant: valid needs residual within a positive threshold, boundary equality is valid', () => {
    const over = { ...artifact(), residual_rms_m: 0.3, threshold_m: 0.05 };
    expect(() => decodeRegistrationArtifact(encodeRaw(over))).toThrow(/threshold/i);
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...artifact(), threshold_m: 0 }))).toThrow(/threshold/i);
    const boundary = { ...artifact(), residual_rms_m: 0.05, threshold_m: 0.05 };
    expect(decodeRegistrationArtifact(encodeRaw(boundary)).residual_rms_m).toBe(0.05);
    expect(toOccupancyRegistration(boundary)?.threshold_m).toBe(0.05);
    // a residual_too_high result stays decodable but unusable
    const tooHigh = { ...artifact(), sensor_to_scan: null, tier: null, valid: false, reason: 'residual_too_high', residual_rms_m: 0.3, per_node: [] };
    expect(decodeRegistrationArtifact(encodeRaw(tooHigh)).valid).toBe(false);
    expect(toOccupancyRegistration(tooHigh)).toBeNull();
  });

  it('round-trips scan_frame_id and treats an absent field as unbound', () => {
    const bound = { ...artifact(), scan_frame_id: 'scan-crc32:0badf00d' };
    expect(decodeRegistrationArtifact(encodeRegistrationArtifact(bound)).scan_frame_id).toBe('scan-crc32:0badf00d');
    const legacy = { ...artifact() } as Record<string, unknown>;
    delete legacy.scan_frame_id;
    expect(decodeRegistrationArtifact(JSON.stringify(legacy)).scan_frame_id).toBeNull();
    expect(() => decodeRegistrationArtifact(encodeRaw({ ...artifact(), scan_frame_id: '' }))).toThrow(/scan_frame_id/);
  });

  it('adapts to the occupancy contract only for an applicable scan and geometry', () => {
    const bound = { ...artifact(), scan_frame_id: 'scan-crc32:aaaaaaaa' };
    expect(toOccupancyRegistration(bound, { geometryComputedAtMs: 1234, scanFrameId: 'scan-crc32:aaaaaaaa' })).not.toBeNull();
    expect(toOccupancyRegistration(bound, { geometryComputedAtMs: 1234, scanFrameId: 'scan-crc32:bbbbbbbb' })).toBeNull();
    expect(toOccupancyRegistration(bound, { geometryComputedAtMs: 9999, scanFrameId: 'scan-crc32:aaaaaaaa' })).toBeNull();
    expect(toOccupancyRegistration(artifact(), { geometryComputedAtMs: 1234, scanFrameId: 'scan-crc32:aaaaaaaa' })).toBeNull();
  });
});
