import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import type { LocalizationRunListing } from '../../src/api/deviceApi';
import { buildRecordingEvidence } from '../../src/domain/recordingEvidence';
import { buildRecordingReport } from '../../src/domain/reportModel';
import { describeFrames } from '../../src/domain/spatialFrames';
import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';
import { RecordingEvidenceView } from '../../src/features/objects/RecordingEvidenceCard';
import ReportView from '../../src/features/report/ReportView';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatDistance } from '../../src/i18n/format';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import { registrationOutcome } from '../../src/xr/fiducial/tagModeState';

/*
 * REMOTE_ONLY (no nRF MAIN): the geophone positions are the AprilTag centres. What the operator and the report say about it:
 * where the positions came from, how they differ from what Channel Sounding measured, and that the registration is not
 * "missing MAIN" for such a session.
 */

beforeAll(async () => { await loadUkrainianCatalog(); });

/* A 1 m square, G1..G4 (pods 1..4). The API reports the positions the localization used. */
function pod(number: number, x: number, y: number, source: 'apriltag_center_approx' | 'channel_sounding') {
  return { pod: number, computed: true, leveled: true, x_mm: x, y_mm: y, z_mm: 0, position_source: source, uncertainty_mm: 0 } as const;
}

function listing(source: 'apriltag_center_approx' | 'channel_sounding'): LocalizationRunListing {
  return {
    object_id: 'OBJ_0001', session_id: 'S000001',
    geometry: { available: true, pods: [pod(1, 0, 0, source), pod(2, 1000, 0, source), pod(3, 1000, 1000, source), pod(4, 0, 1000, source)] },
    cs_pairs: { available: true, pairs: [
      { node_a: 1, node_b: 2, valid: true, distance_mm: 1300, confidence: 900, attempts: 1, valid_samples: 8 },   // tags 1000 -> -0.3 m
      { node_a: 1, node_b: 3, valid: false, distance_mm: 0, confidence: 0, attempts: 3, valid_samples: 0 },        // not measured
      { node_a: 2, node_b: 3, valid: true, distance_mm: 700, confidence: 900, attempts: 1, valid_samples: 8 },     // tags 1000 -> +0.3 m
      { node_a: 3, node_b: 4, valid: true, distance_mm: 1000, confidence: 900, attempts: 1, valid_samples: 8 },    // exact
    ] },
    picks: [], runs: [],
  };
}

function registration(valid: boolean): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: null, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: 4000, created_at_ms: 5000,
    tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
    bindings: [], sensor_to_scan: null, mirrored: false, residual_rms_m: 0, threshold_m: 0.05, per_node: [],
    tier: null, valid, reason: valid ? null : 'missing_main',
  };
}

const block = {
  source: 'APRILTAG_CENTER_APPROX', frame: 'project_z_up_mm',
  positions: [1, 2, 3, 4].map((id) => ({ node_id: id, tag_id: id, x_mm: 0, y_mm: 0, z_mm: 0 })),
} as unknown as NonNullable<RegistrationArtifact['geophone_positions']>;

function render(locale: I18nLocale, element: ReturnType<typeof createElement>): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, element));
}

describe('REMOTE_ONLY evidence of a saved recording', () => {
  it('knows where the positions came from and how they differ from Channel Sounding', () => {
    const evidence = buildRecordingEvidence(listing('apriltag_center_approx'), null, null);
    expect(evidence.positionSource).toBe('apriltag_center_approx');
    expect(evidence.tagResiduals).toHaveLength(6);
    const byPair = new Map(evidence.tagResiduals.map((r) => [`${r.nodeA}-${r.nodeB}`, r]));
    expect(byPair.get('1-2')?.residualM).toBeCloseTo(-0.3, 6);
    expect(byPair.get('2-3')?.residualM).toBeCloseTo(0.3, 6);
    expect(byPair.get('3-4')?.residualM).toBeCloseTo(0, 6);
    expect(byPair.get('1-3')?.bleDistanceM).toBeNull(); // not measured: no difference, never "0 minus 1.414"
    expect(byPair.get('1-3')?.residualM).toBeNull();
    expect(byPair.get('1-4')?.residualM).toBeNull();    // no pair at all
    expect(evidence.worstTagResidualM).toBeCloseTo(0.3, 6);
  });

  it('reports a Channel Sounding session as before, with no tag residuals', () => {
    const evidence = buildRecordingEvidence(listing('channel_sounding'), null, null);
    expect(evidence.positionSource).toBe('channel_sounding');
    expect(evidence.tagResiduals).toEqual([]);
    expect(evidence.worstTagResidualM).toBeNull();
    expect(buildRecordingEvidence(null, null, null).positionSource).toBe('unknown');
  });

  it.each(['en', 'uk'] as const)('shows the position source and the largest difference in %s, and no MAIN complaint', (locale) => {
    const evidence = buildRecordingEvidence(listing('apriltag_center_approx'), { present: true, bytes: 2048 }, null, registration(false));
    const html = render(locale, createElement(RecordingEvidenceView, { evidence }));
    expect(html).toContain(translate(locale, 'evidence.position_source.apriltag'));
    expect(html).toContain(formatDistance(locale, 0.3));
    expect(html).toContain(translate(locale, 'evidence.anchor_tag_centres', { count: 4 }));
    expect(html).not.toContain('missing_main');
    expect(html).not.toContain(translate(locale, 'evidence.anchor_invalid', { reason: 'missing_main' }));
  });

  it('does not add these rows to a Channel Sounding recording', () => {
    const evidence = buildRecordingEvidence(listing('channel_sounding'), null, null, registration(false));
    const html = render('en', createElement(RecordingEvidenceView, { evidence }));
    expect(html).not.toContain(translate('en', 'evidence.position_source.apriltag'));
    expect(html).toContain(translate('en', 'evidence.anchor_invalid', { reason: 'missing_main' }));
  });
});

describe('REMOTE_ONLY registration outcome in the tag panel', () => {
  it.each(['en', 'uk'] as const)('says how many geophone tag centres were taken, in %s', (locale) => {
    const complete = { ...registration(false), geophone_positions: block };
    expect(registrationOutcome(complete, locale)).toBe(translate(locale, 'xr.tag.geophones.complete', { count: 4 }));
    const partial = { ...registration(false), geophone_positions: { ...block, positions: block.positions.filter((p) => p.node_id !== 3) } };
    expect(registrationOutcome(partial, locale)).toBe(translate(locale, 'xr.tag.geophones.partial', { count: 3, missing: 'G3' }));
    expect(registrationOutcome(partial, locale)).not.toContain('MAIN');
  });

  it('keeps the MAIN-based wording for an artifact without positions', () => {
    expect(registrationOutcome(registration(false), 'en')).toBe(
      translate('en', 'xr.tag.registration.failed', { reason: translate('en', 'xr.tag.reason.missing_main') }));
  });
});

describe('report_displays_approximate_marker_origin', () => {
  const location = {
    valid: true, latitude: 50.4501, longitude: 30.5234, accuracy_m: 4,
    altitude_valid: false, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 1, source: 'phone',
  };
  const object: ObjectProfile = { id: 'OBJ_0001', name: 'Bridge pier', active: true, location, cable_lengths_m: [5, 5, 5, 5] };
  const session: RecordingSessionSummary = {
    session_id: 'S000001', complete: true, source: 'AD7606', sample_rate_hz: 500, channel_count: 4,
    samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
  };
  const makeReport = (source: 'apriltag_center_approx' | 'channel_sounding') =>
    buildRecordingReport(object, session, listing(source), null, null, 0);

  it('describes the scan-to-sensor frame as the tag centres for a REMOTE_ONLY recording', () => {
    const frames = describeFrames(location, true, 'en', 'apriltag_center_approx');
    const scanToSensor = frames.transforms.find((t) => t.from === 'scan' && t.to === 'sensor');
    expect(scanToSensor?.status).toBe('identity');
    expect(scanToSensor?.source).toBe(translate('en', 'report.frame.tag_centres'));
    expect(scanToSensor?.note).toBe(translate('en', 'report.frame.scan_to_sensor_tag_centres'));
    // The MAIN-based wording is untouched.
    expect(describeFrames(location, true, 'en').transforms.find((t) => t.from === 'scan')?.status).toBe('unknown');
  });

  it.each(['en', 'uk'] as const)('states that the positions are approximate tag centres and lists the differences in %s', (locale) => {
    const html = render(locale, createElement(ReportView, { report: makeReport('apriltag_center_approx') }));
    expect(html).toContain(translate(locale, 'report.position_source.apriltag'));
    expect(html).toContain(translate(locale, 'report.tag_residuals.heading'));
    expect(html).toContain(translate(locale, 'report.frame.tag_centres'));
    expect(html).toContain(formatDistance(locale, 0.3));
    expect(html).toContain(translate(locale, 'report.tag_residuals.note'));
  });

  it('leaves the report of a Channel Sounding recording exactly without these parts', () => {
    const html = render('en', createElement(ReportView, { report: makeReport('channel_sounding') }));
    expect(html).not.toContain(translate('en', 'report.position_source.apriltag'));
    expect(html).not.toContain(translate('en', 'report.tag_residuals.heading'));
  });
});
