import { describe, expect, it } from 'vitest';

import { decodeLocalizationRun, decodeLocalizationRunListing } from '../../src/api/deviceApi';

describe('MAX-414 localization run decoder', () => {
  it('accepts legacy records with all four provenance fields absent', () => {
    const decoded = decodeLocalizationRun({ schema_version: 5, method_name: 'traveltime_scan_constrained' });
    expect(decoded.schema_version).toBe(5);
    expect(decoded.scan_constraint_applied).toBeUndefined();
    expect(decoded.scan_frame_id).toBeUndefined();
    expect(decoded.occupancy_created_at_ms).toBeUndefined();
    expect(decoded.registration_geometry_computed_at_ms).toBeUndefined();
    expect(decoded.terminal_status).toBe('none');
    expect(decoded.session_epoch_known).toBe(false);
    expect(decoded.session_epoch_timestamp_us).toBeNull();
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

  it('treats a missing runs/picks list as empty but rejects a present non-array one', () => {
    const minimal = decodeLocalizationRunListing({});
    expect(minimal.runs).toEqual([]);
    expect(minimal.picks).toEqual([]);
    expect(minimal.partial_error).toBe(false);
    expect(() => decodeLocalizationRunListing({ runs: {}, picks: [] })).toThrow(/Invalid localization run listing/);
    expect(() => decodeLocalizationRunListing({ runs: [], picks: 'x' })).toThrow(/Invalid localization run listing/);
    expect(() => decodeLocalizationRunListing([])).toThrow(/Invalid localization run listing/);
    expect(() => decodeLocalizationRunListing(null)).toThrow(/Invalid localization run listing/);
  });

  it('decodes handcrafted legacy listing JSON with defaults for every wave-B field', () => {
    const decoded = decodeLocalizationRunListing({
      object_id: 'OBJ_0001', session_id: 'S000001',
      geometry: { available: false, pods: [] }, cs_pairs: { available: false, pairs: [] },
      runs: [{ schema_version: 6, method_name: 'legacy_method' }],
      picks: [{
        schema_version: 1, object_id: 'OBJ_0001', session_id: 'S000001', computed_at_ms: 9,
        channels: [{
          channel: 1, participating: true, status: 'accepted', method: 'sta_lta_refined',
          arrival_sample_index: 10, arrival_subsample_offset: 0, arrival_time_s: 0.01, snr: 2, uncertainty_samples: 1,
        }],
      }],
    });

    expect(decoded.partial_error).toBe(false);
    expect(decoded.partial_errors).toEqual([]);
    expect(decoded.runs[0]).toMatchObject({ terminal_status: 'none', session_epoch_known: false, session_epoch_timestamp_us: null });
    expect(decoded.picks[0]).toMatchObject({ session_epoch_known: false, session_epoch_timestamp_us: null });
    expect(decoded.picks[0].channels[0].s_pick).toBeUndefined();
  });

  it('strictly decodes handcrafted new run, pick, S-pick, epoch and partial-error JSON', () => {
    const decoded = decodeLocalizationRunListing({
      object_id: 'OBJ_0001', session_id: 'S000001',
      geometry: { available: false, pods: [] }, cs_pairs: { available: false, pairs: [] },
      partial_error: true, partial_errors: ['run_record_parse_failed', 'picks_listing_failed'],
      runs: [{
        schema_version: 7, method_name: 'limited_method', terminal_status: 'time_budget_hit',
        session_epoch_known: true, session_epoch_timestamp_us: 1700000000123456,
      }],
      picks: [{
        schema_version: 1, object_id: 'OBJ_0001', session_id: 'S000001', computed_at_ms: 10,
        session_epoch_known: true, session_epoch_timestamp_us: 1700000000123456,
        channels: [{
          channel: 1, participating: true, status: 'rejected_saturated', reason: 'saturated',
          method: 'sta_lta_refined', arrival_sample_index: 20, arrival_subsample_offset: -0.25,
          arrival_time_s: 0.02, snr: 8, uncertainty_samples: 2,
          s_pick: {
            status: 'rejected_ambiguous', reason: 'ambiguous', method: 'sta_lta_refined', sample_index: 30,
            subsample_offset: -0.5, arrival_time_us: 30000, snr: 5, uncertainty_us: 12,
          },
        }],
      }],
    });

    expect(decoded.partial_error).toBe(true);
    expect(decoded.partial_errors).toEqual(['run_record_parse_failed', 'picks_listing_failed']);
    expect(decoded.runs[0].terminal_status).toBe('time_budget_hit');
    expect(decoded.runs[0].session_epoch_timestamp_us).toBe(1700000000123456);
    expect(decoded.picks[0].channels[0]).toMatchObject({ status: 'rejected_saturated', reason: 'saturated' });
    expect(decoded.picks[0].channels[0].s_pick).toMatchObject({
      status: 'rejected_ambiguous', reason: 'ambiguous', sample_index: 30, arrival_time_us: 30000,
    });
  });

  it('rejects unknown terminal and pick status enums deterministically', () => {
    expect(() => decodeLocalizationRun({ schema_version: 7, terminal_status: 'future_status' }))
      .toThrow('Invalid localization run field: terminal_status.');
    expect(() => decodeLocalizationRunListing({
      object_id: 'OBJ_0001', session_id: 'S000001',
      geometry: { available: false, pods: [] }, cs_pairs: { available: false, pairs: [] }, runs: [],
      picks: [{
        schema_version: 1, object_id: 'OBJ_0001', session_id: 'S000001', computed_at_ms: 1,
        channels: [{
          channel: 1, participating: true, status: 'rejected_future', method: 'sta_lta_refined',
          arrival_sample_index: 0, arrival_time_s: 0, snr: 0, uncertainty_samples: 0,
        }],
      }],
    })).toThrow('Invalid localization pick channel field: status.');
  });
});
