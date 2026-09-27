import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { create as createFont } from 'fontkit';
import { createHash } from 'node:crypto';

import ReportView from '../../src/features/report/ReportView';
import { openRecordingReport, renderReportDocument } from '../../src/features/report/reportExport';
import { buildRecordingReport } from '../../src/domain/reportModel';
import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';
import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { uk } from '../../src/i18n/catalog.uk.gen';
import { formatDate, formatNumber } from '../../src/i18n/format';

const api = vi.hoisted(() => ({ fetchLocalizationRuns: vi.fn(), loadObjectScanArtifact: vi.fn() }));
vi.mock('../../src/api/deviceApi', () => api);

afterEach(() => {
  vi.unstubAllGlobals();
  vi.resetAllMocks();
});
beforeAll(async () => { await loadUkrainianCatalog(); });

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

function makeReport(maximal = true, reportObject = object) {
  return buildRecordingReport(
    reportObject,
    session,
    maximal ? listing : null,
    maximal ? new Float32Array([0, 0, 0, 1, 2, 0.5]) : null,
    null,
    0,
  );
}

function render(report: ReturnType<typeof makeReport>, locale: I18nLocale): string {
  return renderToStaticMarkup(createElement(
    LocaleProvider,
    { initialLocale: locale },
    createElement(ReportView, { report }),
  ));
}

function expectNoLeakedKeys(html: string): void {
  const text = html.replace(/<[^>]*>/g, ' ').replace(/&(?:amp|lt|gt|quot|#39);/g, ' ');
  expect(text.match(/(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/g)).toBeNull();
}

function machineModelHash(report: ReturnType<typeof makeReport>): string {
  const machineFields = {
    objectId: report.object.id,
    sessionId: report.session.session_id,
    location: report.object.location,
    links: report.links,
    evidence: report.evidence,
    sectors: report.overlay.sectors.map(({ method, position, radiusM }) => ({ method, position, radiusM })),
    unavailable: report.overlay.unavailable.map(({ method, reason }) => ({ method, reason })),
    qr: report.qr,
    scanPoints: report.scanPoints,
  };
  return createHash('sha256').update(JSON.stringify(machineFields)).digest('hex');
}

function renderedMachineFields(html: string): { ids: string[]; methods: string[]; coordinates: number[] } {
  const row = html.match(/<tr><td>REMOTE 2<\/td>(.*?)<\/tr>/)?.[1] ?? '';
  const cells = [...row.matchAll(/<td>([^<]*)<\/td>/g)].map((match) => match[1]);
  return {
    ids: [object.id, session.session_id],
    methods: ['gauss_newton_tdoa', 'joint', 'traveltime_inversion'].filter((method) => html.includes(method)),
    coordinates: cells.slice(0, 3).map((value) => Number(value.replace(',', '.'))),
  };
}

function qrPaths(html: string): string[] {
  return [...html.matchAll(/<path d="([^"]+)"/g)].map((match) => match[1]);
}

function links(html: string): string[] {
  return [...html.matchAll(/<a href="([^"]+)"/g)].map((match) => match[1]);
}

describe('ReportView', () => {
  it.each(['en', 'uk'] as const)('renders the complete saved report in %s', (locale) => {
    const report = makeReport();
    const html = render(report, locale);
    const labelKeys = [
      'report.location', 'report.coordinate_frames', 'report.recording', 'report.sensor_geometry',
      'report.projections', 'report.scan', 'report.localization_methods', 'report.qr',
    ] as const;
    for (const key of labelKeys) expect(html).toContain(translate(locale, key));
    expect(html).toContain(translate(locale, 'report.title', { object: object.name, session: session.session_id }));
    expect(html).toContain(translate(locale, 'report.metadata', { id: object.id, date: formatDate(locale, report.generatedAtMs) }));
    expect(html).toContain(`${formatNumber(locale, 50.4501, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}, ${formatNumber(locale, 30.5234, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}`);
    expect(html).toContain('https://www.openstreetmap.org/?mlat=50.450100');
    expect(html).toContain('https://www.google.com/maps?q=50.450100,30.523400');
    expect(html).toContain('aria-label="OpenStreetMap"');
    expect(html).toContain('aria-label="Google Maps"');
    for (const key of [
      'report.projection.top_geometry', 'report.projection.side_geometry',
      'report.projection.top_localization', 'report.projection.side_localization',
    ] as const) expect(html).toContain(translate(locale, key));
    expect(html).toContain(translate(locale, 'report.frames_heading_note'));
    expect(html).toContain(translate(locale, 'report.scan_not_registered'));
    expect(html).toContain('gauss_newton_tdoa');
    expect(html).toContain(`joint (${translate(locale, 'report.joint')})`);
    expect(html).toContain(translate(locale, 'report.unavailable_reason', { reason: 'geometry is collinear' }));
    expect(html).toContain('MAX-145');
    const fixed = { minimumFractionDigits: 2, maximumFractionDigits: 2 };
    expect(html).toContain(`<td>REMOTE 2</td><td>${formatNumber(locale, 0, fixed)}</td><td>${formatNumber(locale, 3, fixed)}</td><td>${formatNumber(locale, 0, fixed)}</td><td>${translate(locale, 'report.no')}</td>`);
    for (const value of [2, 1, -0.5]) expect(html).toContain(formatNumber(locale, value, fixed));
    expect(html).toContain(`aria-label="${translate(locale, 'report.result_summary')}"`);
    expect(html).toContain(translate(locale, 'report.qr_full_payload_note'));
    expectNoLeakedKeys(html);
  });

  it.each(['en', 'uk'] as const)('renders a minimal saved report in %s', (locale) => {
    const html = render(makeReport(false), locale);
    expect(html).toContain(translate(locale, 'report.title', { object: object.name, session: session.session_id }));
    expect(html).toContain(translate(locale, 'report.not_calculated'));
    expect(html).toContain(translate(locale, 'report.no_geometry'));
    expect(html).toContain(translate(locale, 'report.no_scan'));
    expectNoLeakedKeys(html);
  });

  it.each(['en', 'uk'] as const)('labels missing sections in %s', (locale) => {
    const noGpsObject = { ...object, location: { ...location, valid: false } };
    const report = buildRecordingReport(noGpsObject, session, null, null, null, 0);
    const html = render(report, locale);
    for (const key of ['report.no_phone_gps', 'report.no_geometry', 'report.no_scan', 'report.not_calculated'] as const) {
      expect(html).toContain(translate(locale, key));
    }
    expect(html).not.toContain('openstreetmap');
    expectNoLeakedKeys(html);
  });

  it('keeps the report model, QR payloads, and map links locale-neutral', () => {
    const modelForEnglish = makeReport();
    const modelForUkrainian = makeReport();
    const english = render(modelForEnglish, 'en');
    const ukrainian = render(modelForUkrainian, 'uk');
    expect(machineModelHash(modelForEnglish)).toBe(machineModelHash(modelForUkrainian));
    expect(modelForEnglish.qr.text).toBe(modelForUkrainian.qr.text);
    expect(links(english)).toEqual(links(ukrainian));
    expect(qrPaths(english)).toEqual(qrPaths(ukrainian));
    expect(renderedMachineFields(english)).toEqual(renderedMachineFields(ukrainian));
    expect(english).not.toBe(ukrainian);
  });

  it.each(['en', 'uk'] as const)('localizes known session statuses and marks unknown statuses in %s', (locale) => {
    const statuses = [
      ['complete', 'report.session_status.complete'],
      ['incomplete', 'report.session_status.incomplete'],
      ['error', 'report.session_status.error'],
      ['recovered_error', 'report.session_status.recovered_error'],
    ] as const;
    for (const [status, key] of statuses) {
      const report = buildRecordingReport(object, { ...session, status }, null, null, null, 0);
      expect(render(report, locale)).toContain(`<tr><th>${translate(locale, 'report.status')}</th><td>${translate(locale, key)}</td></tr>`);
    }

    const rawStatus = 'future-session-state';
    const unknownReport = buildRecordingReport(object, { ...session, status: rawStatus }, null, null, null, 0);
    expect(render(unknownReport, locale)).toContain(
      `<tr><th>${translate(locale, 'report.status')}</th><td>${rawStatus} (${translate(locale, 'report.session_status.unknown')})</td></tr>`,
    );
  });

  it.each(['en', 'uk'] as const)('keeps an unknown remote count distinct from zero in %s', (locale) => {
    const unknown = render(buildRecordingReport(object, session, null, null, null, 0), locale);
    expect(unknown).toContain(
      `<tr><th>${translate(locale, 'report.remote_nodes')}</th><td>${translate(locale, 'report.policy.unknown')} · ${translate(locale, 'report.policy', { value: translate(locale, 'report.policy.unknown') })}</td></tr>`,
    );

    const emptyGeometry: LocalizationRunListing = {
      ...listing, geometry: { available: true, pods: [] },
    };
    const zero = render(buildRecordingReport(object, session, emptyGeometry, null, null, 0), locale);
    expect(zero).toContain(
      `<tr><th>${translate(locale, 'report.remote_nodes')}</th><td>${translate(locale, 'report.remote_count', { count: formatNumber(locale, 0) })} · ${translate(locale, 'report.policy', { value: translate(locale, 'report.policy.insufficient') })}</td></tr>`,
    );
  });

  it.each(['en', 'uk'] as const)('renders a missing report z coordinate as unknown in %s', (locale) => {
    const missingZ: LocalizationRunListing = {
      ...listing,
      geometry: { available: true, pods: [{ pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0 }] },
    };
    const html = render(buildRecordingReport(object, session, missingZ, null, null, 0), locale);
    const fixed = { minimumFractionDigits: 2, maximumFractionDigits: 2 };
    expect(html).toContain(
      `<tr><td>${translate(locale, 'report.main_node')}</td><td>${formatNumber(locale, 0, fixed)}</td><td>${formatNumber(locale, 0, fixed)}</td><td>${translate(locale, 'report.unknown')}</td><td>${translate(locale, 'report.yes')}</td></tr>`,
    );
  });

  it.each(['en', 'uk'] as const)('distinguishes zero GPS coordinates from unavailable location in %s', (locale) => {
    const zero = { ...object, location: { ...location, latitude: 0, longitude: 0 } };
    const html = render(makeReport(false, zero), locale);
    expect(html).toContain(`${formatNumber(locale, 0, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}, ${formatNumber(locale, 0, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}`);
    expect(html).toContain('https://www.openstreetmap.org/?mlat=0.000000');
    const unavailable = render(buildRecordingReport({ ...zero, location: { ...location, valid: false } }, session, null, null, null, 0), locale);
    expect(unavailable).toContain(translate(locale, 'report.no_phone_gps'));
    expect(unavailable).not.toContain('https://www.openstreetmap.org/');
  });

  it('embeds Montserrat WOFF2 data URLs and covers Ukrainian report glyphs', () => {
    const document = renderReportDocument('<article>x</article>', 'Report', 'uk');
    expect(document).toContain('@font-face');
    expect(document).toContain('font-family:Montserrat,sans-serif');
    const urls = [...document.matchAll(/url\('(data:font\/woff2;base64,[^']+)'\)/g)].map((match) => match[1]);
    expect(urls).toHaveLength(6);

    const glyphs = new Set<number>();
    for (const url of urls) {
      const base64 = url.slice(url.indexOf(',') + 1);
      for (const codepoint of createFont(Buffer.from(base64, 'base64')).characterSet) glyphs.add(codepoint);
    }
    const reportText = Object.entries(uk)
      .filter(([key]) => key.startsWith('report.'))
      .map(([, value]) => value.replace(/\{[^}]+\}/g, '0123456789'))
      .join(' ');
    const required = new Set([...reportText, ...'0123456789 ±.,:%—-/>()[];…+−xy mHzsµ²³*\u00a0\u202f']);
    required.delete(String.fromCodePoint(0x202f));
    for (const char of render(makeReport(), 'uk').replace(/<[^>]*>/g, ' ')) required.add(char);
    expect([...required].filter((char) => !glyphs.has(char.codePointAt(0)!))).toEqual([]);
  });

  it('prints the selected catalog label and escapes the document title', () => {
    const doc = renderReportDocument('<article>x</article>', 'A <b>site</b>', 'uk');
    expect(doc.startsWith('<!doctype html>')).toBe(true);
    expect(doc).toContain('<title>A bsite/b</title>');
    expect(doc).toContain(translate('uk', 'report.print_pdf'));
    expect(doc).toContain('window.print()');
    expect(doc).toContain('@media print');
  });

  it('keeps the locale captured at export admission while data loading is pending', async () => {
    let finishListing: (value: LocalizationRunListing) => void = () => undefined;
    api.fetchLocalizationRuns.mockReturnValueOnce(new Promise<LocalizationRunListing>((resolve) => { finishListing = resolve; }));
    api.loadObjectScanArtifact.mockResolvedValueOnce(null);
    const writes: string[] = [];
    const target = { document: { write: (text: string) => writes.push(text), open: vi.fn(), close: vi.fn() } };
    vi.stubGlobal('window', { open: vi.fn(() => target) });

    let currentUiLocale: I18nLocale = 'uk';
    const exportPromise = openRecordingReport(object, session, null, currentUiLocale);
    currentUiLocale = 'en';
    finishListing(listing);
    await exportPromise;

    expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(1);
    expect(api.loadObjectScanArtifact).toHaveBeenCalledTimes(1);
    expect(writes.at(-1)).toContain(translate('uk', 'report.print_pdf'));
    expect(writes.at(-1)).toContain(translate('uk', 'report.coordinate_frames'));
    expect(writes.at(-1)).not.toContain(translate('en', 'report.print_pdf'));
  });
});
