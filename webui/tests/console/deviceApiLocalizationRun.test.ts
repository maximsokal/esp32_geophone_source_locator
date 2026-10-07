import { describe, expect, it } from 'vitest';

import { decodeLocalizationRun } from '../../src/api/deviceApi';

describe('MAX-414 localization run decoder', () => {
  it('accepts legacy records with all four provenance fields absent', () => {
    const decoded = decodeLocalizationRun({ schema_version: 5, method_name: 'traveltime_scan_constrained' });
    expect(decoded.schema_version).toBe(5);
    expect(decoded.scan_constraint_applied).toBeUndefined();
    expect(decoded.scan_frame_id).toBeUndefined();
    expect(decoded.occupancy_created_at_ms).toBeUndefined();
    expect(decoded.registration_geometry_computed_at_ms).toBeUndefined();
  });

  it('preserves schema 6 provenance values including an explicitly unapplied constraint', () => {
    expect(decodeLocalizationRun({
      schema_version: 6,
      method_name: 'eikonal_scan_voids',
      scan_constraint_applied: true,
      scan_frame_id: 'scan-crc32:1a2b3c4d',
      occupancy_created_at_ms: 1234,
      registration_geometry_computed_at_ms: 5678,
    })).toMatchObject({
      schema_version: 6,
      scan_constraint_applied: true,
      scan_frame_id: 'scan-crc32:1a2b3c4d',
      occupancy_created_at_ms: 1234,
      registration_geometry_computed_at_ms: 5678,
    });
    expect(decodeLocalizationRun({ schema_version: 6, scan_constraint_applied: false, scan_frame_id: null,
      occupancy_created_at_ms: 0, registration_geometry_computed_at_ms: 0 }))
      .toMatchObject({ scan_constraint_applied: false, scan_frame_id: null, occupancy_created_at_ms: 0,
        registration_geometry_computed_at_ms: 0 });
  });

  it('rejects malformed present provenance fields instead of treating them as current', () => {
    expect(() => decodeLocalizationRun({ schema_version: 6, scan_constraint_applied: 'true' })).toThrow(/scan_constraint_applied/);
    expect(() => decodeLocalizationRun({ schema_version: 6, scan_frame_id: 17 })).toThrow(/scan_frame_id/);
    expect(() => decodeLocalizationRun({ schema_version: 6, occupancy_created_at_ms: -1 })).toThrow(/occupancy_created_at_ms/);
  });
});
