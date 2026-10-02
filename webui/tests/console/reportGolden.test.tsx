import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { beforeAll, expect, it } from 'vitest';

import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';
import ReportView from '../../src/features/report/ReportView';
import { buildRecordingReport } from '../../src/domain/reportModel';
import { loadUkrainianCatalog, LocaleProvider, type I18nLocale } from '../../src/i18n';
import { formatDate } from '../../src/i18n/format';

beforeAll(async () => { await loadUkrainianCatalog(); });

const generatedAtMs = 1700000000000;
const standardObject: ObjectProfile = {
  id: 'OBJ_0001',
  name: 'Bridge pier',
  active: true,
  location: {
    valid: true, latitude: 50.4501, longitude: 30.5234, accuracy_m: 4,
    altitude_valid: true, altitude_m: 181.2, altitude_accuracy_m: 3, captured_at_ms: 1, source: 'phone',
  },
  cable_lengths_m: [5, 5, 5, 5],
};
const standardSession: RecordingSessionSummary = {
  session_id: 'S000007', status: 'complete', complete: true, source: 'AD7606',
  sample_rate_hz: 500, channel_count: 4, samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
};

function localizationRun(overrides: Partial<LocalizationRun> = {}): LocalizationRun {
  return {
    schema_version: 5, object_id: standardObject.id, session_id: standardSession.session_id,
    computed_at_ms: 1700000000123, kind: 'individual_method', method_name: 'gauss_newton_tdoa', method_version: 3,
    channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'planar_2d', geometry_computed_at_ms: 1699999999000,
    propagation_model_name: 'c', propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 5,
    confidence_score: 0.9, solved: true, source_position: { x_m: 2.25, y_m: 1.5, z_m: -0.5 }, rms_residual_us: 1000,
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

function localizationListing(...runs: LocalizationRun[]): LocalizationRunListing {
  return {
    object_id: standardObject.id,
    session_id: standardSession.session_id,
    geometry: {
      available: true,
      pods: [
        { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
        { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 0 },
        { pod: 2, computed: true, leveled: true, x_mm: 0, y_mm: 3000, z_mm: 0 },
      ],
    },
    cs_pairs: { available: false, pairs: [] },
    picks: [],
    runs,
  };
}

type GoldenState = 'normal' | 'warning' | 'error' | 'zero-value' | 'maximum-value' | 'missing-data';

function fixture(state: GoldenState): {
  object: ObjectProfile;
  session: RecordingSessionSummary;
  listing: LocalizationRunListing | null;
} {
  if (state === 'warning') {
    return {
      object: standardObject,
      session: { ...standardSession, status: 'incomplete', complete: false },
      listing: localizationListing(),
    };
  }
  if (state === 'error') {
    return {
      object: standardObject,
      session: { ...standardSession, status: 'error', complete: false },
      listing: localizationListing(localizationRun({
        solved: false, source_position: undefined, rejection_reason: 'geometry rejected',
      })),
    };
  }
  if (state === 'zero-value') {
    return {
      object: {
        ...standardObject,
        location: {
          valid: true, latitude: 0, longitude: 0, accuracy_m: 0,
          altitude_valid: true, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 0, source: 'phone',
        },
      },
      session: {
        ...standardSession, sample_rate_hz: 0, channel_count: 0, samples_written: 0, dropped_frames: 0, recording_ms: 0,
      },
      listing: localizationListing(localizationRun({
        confidence_score: 0, source_position: { x_m: 0, y_m: 0, z_m: 0 }, rms_residual_us: 0,
      })),
    };
  }
  if (state === 'maximum-value') {
    return {
      object: {
        ...standardObject,
        location: {
          valid: true, latitude: 90, longitude: 180, accuracy_m: 0,
          altitude_valid: true, altitude_m: 8848, altitude_accuracy_m: 0, captured_at_ms: Number.MAX_SAFE_INTEGER, source: 'phone',
        },
      },
      session: {
        ...standardSession, sample_rate_hz: 4294967295, channel_count: 255,
        samples_written: Number.MAX_SAFE_INTEGER, dropped_frames: Number.MAX_SAFE_INTEGER, recording_ms: Number.MAX_SAFE_INTEGER,
      },
      listing: localizationListing(localizationRun({
        source_position: { x_m: 1000000000, y_m: -1000000000, z_m: 1000000000 },
        rms_residual_us: 4294967295,
      })),
    };
  }
  if (state === 'missing-data') {
    return {
      object: { ...standardObject, location: { ...standardObject.location, valid: false } },
      session: {
        ...standardSession, status: 'unknown', complete: false, source: undefined,
        dropped_frames_available: false, recording_ms_available: false,
      },
      listing: null,
    };
  }
  return { object: standardObject, session: standardSession, listing: localizationListing(localizationRun()) };
}

function extractRenderedDomText(html: string, locale: I18nLocale): string {
  const text = html
    .replace(/<[^>]*>/g, ' ')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#x27;/gi, "'")
    .replace(/&#(\d+);/g, (_match, value: string) => String.fromCodePoint(Number(value)))
    .replace(/&#x([0-9a-f]+);/gi, (_match, value: string) => String.fromCodePoint(parseInt(value, 16)))
    .replace(/\s+/gu, ' ')
    .trim();
  return text.replace(formatDate(locale, generatedAtMs).replace(/\s+/gu, ' '), '[generated date]');
}

function renderedMachineData(html: string): string {
  const attributes = [...html.matchAll(/\s(data-[a-z0-9-]+)="([^"]*)"/g)].map(([, key, value]) => [key, value]);
  const urls = [...html.matchAll(/href="([^"]+)"/g)].map(([, url]) => url);
  const payloads = [...html.matchAll(/data-payload="([^"]+)"/g)].map(([, payload]) => payload);
  const qrPaths = [...html.matchAll(/<path d="([^"]+)"/g)].map(([, path]) => path);
  return JSON.stringify({ attributes, urls, payloads, qrPaths });
}

function machineDataHash(value: string): string {
  return createHash('sha256').update(value).digest('hex');
}

function renderReport(report: ReturnType<typeof buildRecordingReport>, locale: I18nLocale): string {
  return renderToStaticMarkup(createElement(
    LocaleProvider,
    { initialLocale: locale },
    createElement(ReportView, { report }),
  ));
}

const states: GoldenState[] = ['normal', 'warning', 'error', 'zero-value', 'maximum-value', 'missing-data'];

it.each(states)('matches the EN and UK %s rendered report text goldens and keeps machine data identical', (state) => {
  const data = fixture(state);
  const report = buildRecordingReport(data.object, data.session, data.listing, null, null, generatedAtMs);
  const englishHtml = renderReport(report, 'en');
  const ukrainianHtml = renderReport(report, 'uk');

  expect(machineDataHash(renderedMachineData(englishHtml))).toBe(machineDataHash(renderedMachineData(ukrainianHtml)));

  for (const [locale, html] of [['en', englishHtml], ['uk', ukrainianHtml]] as const) {
    // The export path produces print-ready HTML; no PDF is generated in this test.
    // These fixtures golden normalized visible text from ReportView's rendered DOM.
    const actualText = extractRenderedDomText(html, locale);
    const goldenUrl = new URL('./fixtures/i18n-golden/' + state + '.' + locale + '.txt', import.meta.url);
    const expectedText = readFileSync(goldenUrl, 'utf8').trimEnd();
    expect(actualText).toBe(expectedText);
  }
});
