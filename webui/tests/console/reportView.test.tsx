import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import ReportView from '../../src/features/report/ReportView';
import { renderReportDocument } from '../../src/features/report/reportExport';
import { buildRecordingReport } from '../../src/domain/reportModel';
import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';
import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';

/* MAX-150: printable report of one saved recording. */

const location = {
  valid: true, latitude: 50.4501, longitude: 30.5234, accuracy_m: 4,
  altitude_valid: true, altitude_m: 181.2, altitude_accuracy_m: 3, captured_at_ms: 1, source: 'phone',
};
const object: ObjectProfile = { id: 'OBJ_0001', name: 'Bridge pier', active: true, location, cable_lengths_m: [5, 5, 5, 5] };
const session: RecordingSessionSummary = {
  session_id: 'S000007', complete: true, source: 'AD7606', sample_rate_hz: 500, channel_count: 4,
  samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
};

function run(method: string, overrides: Partial<LocalizationRun> = {}): LocalizationRun {
  return {
    schema_version: 1, object_id: 'OBJ_0001', session_id: 'S000007', computed_at_ms: 5,
    kind: 'individual_method', method_name: method, method_version: 1, channel_mask_used: 15,
    channel_mask_rejected: 0, dimensionality: 'planar_2d', geometry_computed_at_ms: 1,
    propagation_model_name: 'c', propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 5,
    confidence_score: 0.9, solved: true, source_position: { x_m: 2, y_m: 1, z_m: -0.5 }, rms_residual_us: 1000,
    ...overrides,
  };
}

const listing: LocalizationRunListing = {
  object_id: 'OBJ_0001', session_id: 'S000007',
  geometry: { available: true, pods: [
    { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
    { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 0 },
    { pod: 2, computed: true, leveled: false, x_mm: 0, y_mm: 3000, z_mm: 0 },
  ] },
  cs_pairs: { available: false, pairs: [] }, picks: [],
  runs: [
    run('gauss_newton_tdoa'),
    run('joint', { kind: 'joint_fusion', source_position: { x_m: 2.2, y_m: 1.1, z_m: -0.4 } }),
    run('traveltime_inversion', { solved: false, source_position: undefined, rejection_reason: 'geometry is collinear' }),
  ],
};

describe('ReportView', () => {
  it('renders every required section from the saved recording', () => {
    const report = buildRecordingReport(object, session, listing, new Float32Array([0, 0, 0, 1, 2, 0.5]), null, 0);
    const html = renderToStaticMarkup(createElement(ReportView, { report }));
    expect(html).toContain('Bridge pier — recording S000007');
    expect(html).toContain('no live acquisition used');
    /* GPS + both map links + their QR codes. */
    expect(html).toContain('50.450100, 30.523400');
    expect(html).toContain('https://www.openstreetmap.org/?mlat=50.450100');
    expect(html).toContain('https://www.google.com/maps?q=50.450100,30.523400');
    expect(html).toContain('aria-label="OpenStreetMap"');
    expect(html).toContain('aria-label="Google Maps"');
    /* Geometry + levelling. */
    expect(html).toContain('<td>Pod 2</td><td>0.00</td><td>3.00</td><td>0.00</td><td>no</td>');
    /* Four sensor-frame projections: with and without localization. */
    expect(html).toContain('Top (x/y), geometry only');
    expect(html).toContain('Side (x/z), with localization');
    /* Scan drawn in its own frame, explicitly not registered. */
    expect(html).toContain('Scan, top');
    expect(html).toContain('MAX-145');
    /* Every method separately, the joint result marked, failures with reason. */
    expect(html).toContain('gauss_newton_tdoa');
    expect(html).toContain('joint (joint)');
    expect(html).toContain('unavailable: geometry is collinear');
    /* MAX-151: frames are explicit; heading never from GPS. */
    expect(html).toContain('Heading (true north) is not known');
    expect(html).toContain('translation only (±4 m)');
    /* Report QR. */
    expect(html).toContain('aria-label="Result summary"');
  });

  it('states missing data instead of inventing it', () => {
    const report = buildRecordingReport({ ...object, location: { ...location, valid: false } }, session, null, null, null, 0);
    const html = renderToStaticMarkup(createElement(ReportView, { report }));
    expect(html).toContain('No phone GPS location stored');
    expect(html).toContain('No geometry was frozen with this recording.');
    expect(html).toContain('No 3D scan saved for this recording.');
    expect(html).toContain('Not calculated.');
    expect(html).not.toContain('openstreetmap');
  });

  it('wraps the report in a printable document with a safe title', () => {
    const doc = renderReportDocument('<article>x</article>', 'A <b>site</b>');
    expect(doc.startsWith('<!doctype html>')).toBe(true);
    expect(doc).toContain('<title>A bsite/b</title>');
    expect(doc).toContain('window.print()');
    expect(doc).toContain('@media print');
  });
});
