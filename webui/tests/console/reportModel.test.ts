import { describe, expect, it } from 'vitest';

import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';
import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import {
  buildRecordingReport,
  makeProjection,
  mapLinks,
  reportQr,
  thinPoints,
} from '../../src/domain/reportModel';
import { buildLocalizationOverlay } from '../../src/domain/localizationOverlay';

/* MAX-150: report model. */

const location = {
  valid: true, latitude: 50.4501, longitude: 30.5234, accuracy_m: 5,
  altitude_valid: false, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 1, source: 'phone',
};
const object: ObjectProfile = { id: 'OBJ_0001', name: 'Test site', active: true, location, cable_lengths_m: [5, 5, 5, 5] };
const session: RecordingSessionSummary = {
  session_id: 'S000001', complete: true, sample_rate_hz: 500, channel_count: 4,
  samples_written: 1000, dropped_frames: 0, recording_ms: 2000,
};

function run(method: string, x: number): LocalizationRun {
  return {
    schema_version: 1, object_id: 'OBJ_0001', session_id: 'S000001', computed_at_ms: 5,
    kind: 'individual_method', method_name: method, method_version: 1, channel_mask_used: 15,
    channel_mask_rejected: 0, dimensionality: 'planar_2d', geometry_computed_at_ms: 1,
    propagation_model_name: 'c', propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 5,
    confidence_score: 0.9, solved: true, source_position: { x_m: x, y_m: 1, z_m: -0.5 }, rms_residual_us: 1000,
  };
}

const listing: LocalizationRunListing = {
  object_id: 'OBJ_0001', session_id: 'S000001',
  geometry: { available: true, pods: [
    { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
    { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 0 },
  ] },
  cs_pairs: { available: false, pairs: [] }, picks: [],
  runs: [run('gauss_newton_tdoa', 2), run('semblance_scan', 3)],
};

describe('report model', () => {
  it('builds map links only from a valid stored location', () => {
    expect(mapLinks(location)).toEqual({
      osm: 'https://www.openstreetmap.org/?mlat=50.450100&mlon=30.523400#map=18/50.450100/30.523400',
      google: 'https://www.google.com/maps?q=50.450100,30.523400',
    });
    expect(mapLinks({ ...location, valid: false })).toBeNull();
    expect(mapLinks({ ...location, latitude: 120 })).toBeNull();
    expect(mapLinks(undefined)).toBeNull();
  });

  it('projects with equal aspect and up on paper', () => {
    const top = makeProjection([{ x: 0, y: 0, z: 0 }, { x: 10, y: 5, z: 0 }], 'x', 'y', 220, 120, 10);
    const a = top.map(0, 0, 0);
    const b = top.map(10, 5, 0);
    expect(b.u).toBeGreaterThan(a.u);
    expect(b.v).toBeLessThan(a.v); /* +y is up */
    expect(Math.abs((b.u - a.u) / 10 - (a.v - b.v) / 5)).toBeLessThan(1e-9); /* same scale */
    expect(a.u).toBeGreaterThanOrEqual(10);
    expect(b.u).toBeLessThanOrEqual(210);
    const side = makeProjection([{ x: 0, y: 0, z: -2 }, { x: 4, y: 9, z: 1 }], 'x', 'z', 200, 200);
    expect(side.map(0, 0, 1).v).toBeLessThan(side.map(0, 0, -2).v);
    /* No points: a sane default frame. */
    expect(Number.isFinite(makeProjection([], 'x', 'y', 100, 100).scale)).toBe(true);
  });

  it('thins a point cloud evenly and drops non-finite points', () => {
    const cloud = new Float32Array(3000 * 3).map((_, i) => i);
    expect(thinPoints(cloud, 1000).length).toBe(1000);
    const bad = new Float32Array([0, 0, 0, Number.NaN, 1, 1, 2, 2, 2]);
    expect(thinPoints(bad, 10)).toEqual([{ x: 0, y: 0, z: 0 }, { x: 2, y: 2, z: 2 }]);
  });

  it('puts the selected result into the QR when it fits, else a reference with the limitation', () => {
    const overlay = buildLocalizationOverlay(listing.runs, listing.geometry, 'semblance_scan');
    const qr = reportQr(object, 'S000001', overlay);
    expect(qr.kind).toBe('full_payload');
    expect(JSON.parse(qr.text)).toEqual({ o: 'OBJ_0001', s: 'S000001', m: 'semblance_scan', p: [3, 1, -0.5], r: 0.3, n: 2 });
    const long = { ...object, id: 'X'.repeat(300) };
    const fallback = reportQr(long, 'S000001', overlay);
    expect(fallback.kind).toBe('reference');
    expect(fallback.text).toBe(`geolocator:object=${long.id};session=S000001`);
  });

  it('assembles one recording without live data', () => {
    const report = buildRecordingReport(object, session, listing, new Float32Array([0, 0, 0, 1, 1, 1]), null, 42);
    expect(report.links).not.toBeNull();
    expect(report.overlay.sectors).toHaveLength(2);
    expect(report.evidence.scan).toBe('available');
    expect(report.scanPoints).toHaveLength(2);
    const noScan = buildRecordingReport(object, session, null, null, null, 42);
    expect(noScan.evidence.scan).toBe('absent');
    expect(noScan.overlay.sectors).toHaveLength(0);
  });
});
