import { beforeAll, describe, expect, it } from 'vitest';
import { createElement, type ReactNode } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import DeviceDashboard from '../../src/features/dashboard/DeviceDashboard';
import PowerBanner from '../../src/components/PowerBanner';
import LevelingCard from '../../src/features/sensors/LevelingCard';
import LocalizationOverlay from '../../src/features/objects/LocalizationOverlay';
import { RecordingEvidenceView } from '../../src/features/objects/RecordingEvidenceCard';
import ReportView from '../../src/features/report/ReportView';
import { renderReportDocument } from '../../src/features/report/reportExport';
import { buildRecordingEvidence } from '../../src/domain/recordingEvidence';
import { buildRecordingReport } from '../../src/domain/reportModel';
import { sectorRadius } from '../../src/domain/localizationOverlay';
import type { DeviceStatus, ObjectProfile, RecordingGateStatus, RecordingSessionSummary } from '../../src/domain/device';
import { estimatedVelocityText } from '../../src/features/objects/LocalizationCalculatePanel';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';

beforeAll(async () => { await loadUkrainianCatalog(); });

const object: ObjectProfile = {
  id: 'OBJ_0001', name: 'Bridge pier', active: true,
  location: {
    valid: true, latitude: 50.4501, longitude: 30.5234, accuracy_m: 4,
    altitude_valid: true, altitude_m: 181.2, altitude_accuracy_m: 3, captured_at_ms: 1, source: 'phone',
  },
  cable_lengths_m: [5, 5, 5, 5],
};
const session: RecordingSessionSummary = {
  session_id: 'S000007', complete: true, source: 'AD7606', sample_rate_hz: 500, channel_count: 4,
  samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
};
const geometry: LocalizationRunListing['geometry'] = {
  available: true,
  pods: [
    { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
    { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 0 },
    { pod: 2, computed: true, leveled: true, x_mm: 0, y_mm: 3000, z_mm: 0 },
  ],
};

function run(overrides: Partial<LocalizationRun> = {}): LocalizationRun {
  return {
    schema_version: 5, object_id: object.id, session_id: session.session_id, computed_at_ms: 1700000000123,
    kind: 'individual_method', method_name: 'gauss_newton_tdoa', method_version: 3,
    channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'planar_2d', geometry_computed_at_ms: 1699999999000,
    propagation_model_name: 'c', propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 5,
    confidence_score: 0.9, solved: true,
    source_position: { x_m: 2.25, y_m: 1.5, z_m: -0.5 }, rms_residual_us: 1000,
    estimated_p_velocity_mps: 1234.5,
    velocity_range: { v_min_mps: 150, v_max_mps: 1200, vp_vs_min: 0, vp_vs_max: 0, source: 'user' },
    velocity_status: 'ok',
    uncertainty: {
      std_m: { x_m: 0.12, y_m: 0.23, z_m: 0.34 }, credible_level: 0.9,
      credible_volume_m3: 0.01, credible_area_m2: 0,
      credible_min: { x_m: 2, y_m: 1, z_m: -1 }, credible_max: { x_m: 2.5, y_m: 2, z_m: 0 },
    },
    ...overrides,
  };
}

function listing(...runs: LocalizationRun[]): LocalizationRunListing {
  return { object_id: object.id, session_id: session.session_id, geometry, cs_pairs: { available: false, pairs: [] }, picks: [], runs };
}

function deviceStatus(systemState: string | null, power: Record<string, unknown> = {}): DeviceStatus | null {
  if (systemState === null) return null;
  return {
    system: { state: systemState, mode: 'SEISMIC', uptime_ms: 1000, acquisition_ready: true, ready_pods: 4, required_pods: 4 },
    power: {
      valid: true, voltage_v: 4, current_a: 0.1, power_w: 0.4,
      state: 'BATTERY', low_battery: 'normal', battery_percent_valid: true, battery_percent_estimate: 64,
      ...power,
    },
    environment: { valid: true, temperature_c: 20, humidity_percent: 40, pressure_pa: 100000 },
    mux: { valid: true, active_channel: 0, active_pod: 0 },
    pods: [],
    acquisition: {
      running: true, requested_running: true, hardware_ready: true, source: 'AD7606',
      last_error: 0, last_error_name: 'ESP_OK', sample_rate_hz: 500, samples_recorded: 12000, recording_ms: 24000,
    },
  } as DeviceStatus;
}

interface Fixture {
  id: string;
  systemState: string | null;
  sessionStatus: NonNullable<RecordingSessionSummary['status']>;
  runs: LocalizationRun[] | null;
  power?: Record<string, unknown>;
  gate?: RecordingGateStatus;
}

const fixtures: Fixture[] = [
  { id: 'normal', systemState: 'READY', sessionStatus: 'complete', runs: [run()] },
  { id: 'warning', systemState: 'DEGRADED', sessionStatus: 'incomplete', runs: [] },
  { id: 'error', systemState: 'FAULT', sessionStatus: 'error', runs: [run({ solved: false, source_position: undefined, rejection_reason: 'geometry rejected' })] },
  { id: 'source-state-disagreement', systemState: 'READY', sessionStatus: 'error', runs: [] },
  { id: 'unavailable-stale', systemState: null, sessionStatus: 'unknown', runs: null },
  { id: 'low-battery', systemState: 'READY', sessionStatus: 'complete', runs: [run()], power: { state: 'BATTERY_LOW', low_battery: 'warning', battery_percent_estimate: 18 } },
  {
    id: 'recording-gate', systemState: 'READY', sessionStatus: 'incomplete', runs: [],
    gate: {
      available: true, assigned_count: 3, accepted_count: 2, allowed: false, decision: 'BLOCKED_NOT_LEVEL',
      reason: 'Level every connected geophone before recording.', nodes: [{ channel: 0, node_id: 7, state: 'tilted', tilt_deg: 12.34 }],
    },
  },
  { id: 'localization-solved', systemState: 'READY', sessionStatus: 'complete', runs: [run()] },
  { id: 'localization-rejected', systemState: 'READY', sessionStatus: 'incomplete', runs: [run({ solved: false, source_position: undefined, rejection_reason: 'geometry is collinear' })] },
  { id: 'localization-unverified', systemState: 'READY', sessionStatus: 'unknown', runs: null },
  { id: 'velocity-unknown', systemState: 'READY', sessionStatus: 'complete', runs: [run({ estimated_p_velocity_mps: undefined, velocity_range: undefined, velocity_status: 'unknown' })] },
  { id: 'velocity-inconsistent', systemState: 'READY', sessionStatus: 'complete', runs: [run({ velocity_status: 'inconsistent' })] },
  { id: 'velocity-at-bound', systemState: 'READY', sessionStatus: 'complete', runs: [run({ velocity_status: 'at_bound' })] },
];

function renderInLocale(node: ReactNode, locale: I18nLocale): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, node));
}

function renderFixture(fixture: Fixture, locale: I18nLocale) {
  const status = deviceStatus(fixture.systemState, fixture.power);
  const runs = fixture.runs;
  const savedRuns = runs === null ? null : listing(...runs);
  const evidence = buildRecordingEvidence(savedRuns, null, null);
  const report = buildRecordingReport(
    object, { ...session, status: fixture.sessionStatus }, savedRuns, null, null, 1700000000999,
  );
  const callbacks = {
    onOpenObjects: () => undefined, onOpenSensors: () => undefined,
    onOpenActiveObject: () => undefined, onRefresh: () => undefined,
  };
  const dashboard = renderInLocale(createElement(DeviceDashboard, { status, objects: null, error: null, ...callbacks }), locale);
  const power = renderInLocale(createElement(PowerBanner, { status }), locale);
  const gate = fixture.gate === undefined ? '' : renderInLocale(createElement(LevelingCard, {
    gate: fixture.gate, pods: [], onCalibrateHeights: async () => true,
  }), locale);
  const overlay = savedRuns === null || runs === null || runs.length === 0 ? '' : renderInLocale(createElement(LocalizationOverlay, {
    runs, geometry, renderScene: false,
  }), locale);
  const evidenceView = renderInLocale(createElement(RecordingEvidenceView, { evidence }), locale);
  const reportView = renderInLocale(createElement(ReportView, { report }), locale);
  return { dashboard, power, gate, overlay, evidenceView, reportView, report };
}

function localizationRowStates(html: string): string[] {
  return [...html.matchAll(/<tr[^>]*data-state="([^"]+)" data-severity="([^"]+)"[^>]*>/g)]
    .map(([, state, severity]) => `${state}:${severity}`);
}

function localizationMachineRows(html: string): string[][] {
  return [...html.matchAll(/<tr[^>]*data-method-id="[^"]+"[^>]*>/g)].map(([row]) =>
    [...row.matchAll(/\s(data-[a-z0-9-]+)="([^"]*)"/g)].map(([, key, value]) => `${key}=${value}`));
}

function renderedMachineData(html: string): string {
  const attributes = [...html.matchAll(/\s(data-[a-z0-9-]+)="([^"]*)"/g)].map(([, key, value]) => [key, value]);
  const urls = [...html.matchAll(/href="([^"]+)"/g)].map(([, url]) => url);
  const qrPaths = [...html.matchAll(/<path d="([^"]+)"/g)].map(([, path]) => path);
  return JSON.stringify({ attributes, urls, qrPaths });
}

function machineFields(report: ReturnType<typeof buildRecordingReport>, fixture: Fixture): string {
  const method = fixture.runs?.[0];
  return JSON.stringify({
    object_id: report.object.id,
    session_id: report.session.session_id,
    method_id: method?.method_name ?? null,
    method_version: method?.method_version ?? null,
    coordinates: method?.source_position ?? null,
    uncertainty: method?.uncertainty ?? null,
    velocity: {
      estimated_p_velocity_mps: method?.estimated_p_velocity_mps ?? null,
      range: method?.velocity_range ?? null,
    },
    timestamps: [report.generatedAtMs, method?.computed_at_ms ?? null, method?.geometry_computed_at_ms ?? null],
    qr_payload: report.qr.text,
    urls: report.links,
  });
}

describe('MAX-189 WebUI/report semantic fixture matrix', () => {
  it.each(['en', 'uk'] as const)('renders neutral status fixtures in %s with matching state and severity', (locale) => {
    for (const fixture of fixtures) {
      const rendered = renderFixture(fixture, locale);
      const webEvidenceState = rendered.evidenceView.match(/data-localization-state="([^"]+)" data-severity="([^"]+)"/);
      const reportEvidenceState = rendered.reportView.match(/data-localization-state="([^"]+)" data-severity="([^"]+)"/);
      expect(webEvidenceState?.slice(1)).toEqual(reportEvidenceState?.slice(1));
      if (['normal', 'warning', 'error', 'unavailable-stale'].includes(fixture.id)) {
        const dashboardStatus = rendered.dashboard.match(/data-state="([^"]+)" data-severity="([^"]+)"/);
        const reportStatus = rendered.reportView.match(/<header data-state="([^"]+)" data-severity="([^"]+)"/);
        expect(dashboardStatus?.slice(1)).toEqual(reportStatus?.slice(1));
      }

      if (fixture.runs !== null && fixture.runs.length > 0) {
        expect(localizationRowStates(rendered.overlay)).toEqual(localizationRowStates(rendered.reportView));
        expect(localizationMachineRows(rendered.overlay)).toEqual(localizationMachineRows(rendered.reportView));
      }
      if (fixture.id === 'low-battery') {
        expect(rendered.power).toContain('role="alert"');
        expect(rendered.power).toContain(locale === 'en' ? 'Battery low: connect USB power soon.' : 'Низький заряд акумулятора: невдовзі під’єднайте живлення USB.');
      }
      if (fixture.id === 'recording-gate') {
        expect(rendered.gate).toContain('class="blocking"');
        expect(rendered.gate).toContain(locale === 'en' ? 'TILTED' : 'НАХИЛЕНО');
      }
      if (fixture.id === 'localization-solved') {
        expect(rendered.evidenceView).toContain(translate(locale, 'evidence.localization.calculated'));
      }
      if (fixture.id === 'localization-unverified') {
        expect(rendered.evidenceView).toContain(translate(locale, 'evidence.localization.unknown'));
      }
      if (fixture.id === 'velocity-inconsistent') {
        expect(localizationRowStates(rendered.overlay)).toContain('velocity_inconsistent:warning');
      }
      if (fixture.id === 'velocity-at-bound') {
        expect(localizationRowStates(rendered.overlay)).toContain('velocity_at_bound:warning');
      }
      if (fixture.id === 'velocity-unknown') {
        expect(rendered.reportView).toContain('data-estimated-p-velocity-mps=""');
        expect(rendered.reportView).toContain('data-velocity-min-mps=""');
        expect(rendered.reportView).toContain('data-velocity-max-mps=""');
        expect(rendered.reportView).not.toContain('data-estimated-p-velocity-mps="0"');
      }
    }
  });

  it('keeps dashboard and report states tied to their own inputs', () => {
    const fixture = fixtures.find(({ id }) => id === 'source-state-disagreement')!;
    const rendered = renderFixture(fixture, 'en');
    // Dashboard reflects device.system.state; the report header reflects saved session.status.
    const dashboardStatus = rendered.dashboard.match(/data-state="([^"]+)" data-severity="([^"]+)"/);
    const reportStatus = rendered.reportView.match(/<header data-state="([^"]+)" data-severity="([^"]+)"/);
    expect(dashboardStatus?.slice(1)).toEqual(['normal', 'normal']);
    expect(reportStatus?.slice(1)).toEqual(['error', 'error']);
  });

  it('keeps identifiers, method metadata, coordinates, uncertainty, timestamps, velocity range, QR, and URLs byte-identical by locale', () => {
    for (const fixture of fixtures) {
      const english = renderFixture(fixture, 'en');
      const ukrainian = renderFixture(fixture, 'uk');
      expect(machineFields(english.report, fixture)).toBe(machineFields(ukrainian.report, fixture));
      expect(renderedMachineData(english.reportView)).toBe(renderedMachineData(ukrainian.reportView));
      if (fixture.runs?.[0] !== undefined) {
        for (const html of [english.reportView, ukrainian.reportView]) {
          expect(html).toContain(fixture.runs[0].method_name);
          expect(html).toContain(object.id);
          expect(html).toContain(session.session_id);
        }
      }
    }
  });

  it.each(['en', 'uk'] as const)('does not render an unknown battery percentage as zero in %s', (locale) => {
    const invalid = deviceStatus('READY', { battery_percent_valid: false, battery_percent_estimate: 0 });
    const missing = deviceStatus('READY', { battery_percent_valid: false, battery_percent_estimate: null });
    const validZero = deviceStatus('READY', { battery_percent_valid: true, battery_percent_estimate: 0 });
    const dashboard = (status: DeviceStatus | null) => renderInLocale(createElement(DeviceDashboard, {
      status, objects: null, error: null, onOpenObjects: () => undefined, onOpenSensors: () => undefined,
      onOpenActiveObject: () => undefined, onRefresh: () => undefined,
    }), locale);
    const label = translate(locale, 'dashboard.battery_percent');
    const unknown = translate(locale, 'power.state_unknown');
    const unknownCard = `<article class="console-card metric-card"><span>${label}</span><strong>${unknown}</strong></article>`;
    const zeroCard = `<article class="console-card metric-card"><span>${label}</span><strong>${formatNumber(locale, 0, { maximumFractionDigits: 0 })}%</strong></article>`;
    expect(dashboard(invalid)).toContain(unknownCard);
    expect(dashboard(missing)).toContain(unknownCard);
    expect(dashboard(validZero)).toContain(zeroCard);
  });

  it.each(['en', 'uk'] as const)('keeps unknown estimated velocity and range distinct from zero in %s', (locale) => {
    expect(estimatedVelocityText({}, locale)).toBe('');
    expect(estimatedVelocityText({ estimated_p_velocity_mps: 0 }, locale)).toBe('');
    const estimated = estimatedVelocityText({ estimated_p_velocity_mps: 1234.5 }, locale);
    expect(estimated).toContain(formatNumber(locale, 1235));
    const noVelocityRange = run({ velocity_range: undefined, uncertainty: undefined, rms_residual_us: 1000, propagation_speed_mps: 300 });
    expect(sectorRadius(noVelocityRange).radiusM).toBeCloseTo(0.3);
    expect(sectorRadius(run({ uncertainty: undefined, velocity_range: { v_min_mps: 150, v_max_mps: 1200, vp_vs_min: 0, vp_vs_max: 0, source: 'user' } })).radiusM).toBeCloseTo(1.2);
  });

  it('declares a fallback font-family and preserves report text when font URLs are replaced', () => {
    const report = buildRecordingReport(object, session, listing(run()), null, null, 1700000000999);
    const rendered = renderInLocale(createElement(ReportView, { report }), 'uk');
    const document = renderReportDocument(rendered, 'Report', 'uk');
    const replacedFontUrls = document.replace(/url\('[^']+'\)/g, "url('broken-font.woff2')");
    expect(document).toContain('font-family:Montserrat,sans-serif');
    expect(replacedFontUrls).toContain('font-family:Montserrat,sans-serif');
    expect(replacedFontUrls.slice(replacedFontUrls.indexOf('<body>'))).toContain(rendered);
    expect(rendered).toContain(translate('uk', 'report.location'));
    expect(rendered).toContain('OBJ_0001');
    expect(rendered).toContain('S000007');
    expect(rendered).toContain('gauss_newton_tdoa');
    expect(report.qr.text).toContain('OBJ_0001');
  });
});
