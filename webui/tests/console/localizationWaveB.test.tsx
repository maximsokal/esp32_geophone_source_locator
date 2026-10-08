import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({
  fetchLocalizationJob: vi.fn(),
  fetchLocalizationRuns: vi.fn(),
  fetchScanInfo: vi.fn(),
  getCalcSettings: vi.fn(),
  sendControl: vi.fn(),
  saveCalcSettings: vi.fn(),
}));
vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../src/api/deviceApi')>(),
  ...api,
}));

import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import LocalizationCalculatePanel from '../../src/features/objects/LocalizationCalculatePanel';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';

beforeAll(async () => { await loadUkrainianCatalog(); });
afterEach(() => { vi.unstubAllGlobals(); vi.resetAllMocks(); });

function run(
  method_name: string,
  rejection_reason: string,
  terminal_status: LocalizationRun['terminal_status'],
  computed_at_ms: number,
): LocalizationRun {
  return {
    schema_version: 7,
    object_id: 'OBJ_0001',
    session_id: 'S000001',
    computed_at_ms,
    kind: 'individual_method',
    method_name,
    method_version: 1,
    channel_mask_used: 15,
    channel_mask_rejected: 0,
    dimensionality: 'insufficient',
    geometry_computed_at_ms: 1,
    propagation_model_name: 'custom',
    propagation_speed_mps: 340,
    propagation_speed_uncertainty_mps: 5,
    confidence_score: 0,
    solved: false,
    terminal_status,
    session_epoch_known: false,
    session_epoch_timestamp_us: null,
    rejection_reason,
  };
}

function listing(): LocalizationRunListing {
  return {
    object_id: 'OBJ_0001',
    session_id: 'S000001',
    geometry: { available: false, pods: [] },
    cs_pairs: { available: false, pairs: [] },
    partial_error: true,
    partial_errors: ['run_record_parse_failed'],
    runs: [
      run('iteration_method', 'session_gap', 'iteration_limit', 1),
      run('skipped_method', 'timestamp_non_monotonic', 'time_budget_skipped', 2),
      run('budget_method', 'duplicate_sensor_position', 'time_budget_hit', 3),
      run('cell_method', 'reflected_transform', 'cell_budget', 4),
      run('resolution_method', 'session_gap', 'resolution_limited', 5),
    ],
    picks: [{
      schema_version: 1,
      object_id: 'OBJ_0001',
      session_id: 'S000001',
      computed_at_ms: 8,
      session_epoch_known: true,
      session_epoch_timestamp_us: 1700000000123456,
      channels: [
        {
          channel: 1, participating: true, status: 'rejected_saturated', reason: 'saturated',
          method: 'sta_lta_refined', arrival_sample_index: 50, arrival_subsample_offset: -0.25,
          arrival_time_s: 0.05, snr: 9.5, uncertainty_samples: 2,
          s_pick: {
            status: 'accepted', method: 'sta_lta_refined', sample_index: 150,
            subsample_offset: -0.5, arrival_time_us: 150000, snr: 6.5, uncertainty_us: 18,
          },
        },
        {
          channel: 2, participating: true, status: 'rejected_ambiguous', reason: 'ambiguous',
          method: 'sta_lta_refined', arrival_sample_index: 70, arrival_subsample_offset: 0,
          arrival_time_s: 0.07, snr: 4.5, uncertainty_samples: 3,
        },
      ],
    }],
  };
}

function textContent(value: unknown): string[] {
  if (typeof value === 'string') return [value];
  if (Array.isArray(value)) return value.flatMap(textContent);
  if (typeof value === 'object' && value !== null && 'children' in value) {
    return textContent((value as { children: unknown }).children);
  }
  return [];
}

async function renderPanel(value: LocalizationRunListing, locale: I18nLocale): Promise<ReactTestRenderer> {
  api.fetchLocalizationRuns.mockResolvedValueOnce(value);
  api.fetchScanInfo.mockResolvedValueOnce({ present: false, bytes: 0 });
  api.getCalcSettings.mockResolvedValueOnce({ settings: null, presets: [], standardProcessing: {} });
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: locale }, createElement(LocalizationCalculatePanel, {
      objectId: value.object_id, sessionId: value.session_id, busy: false, onClose: () => undefined,
    })));
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
  return renderer;
}

function dispose(renderer: ReactTestRenderer): void {
  act(() => renderer.unmount());
}

describe('MAX-132 wave-B localization listing', () => {
  it('renders partial warnings, terminal outcomes, rejection reasons, S-picks and epoch in EN and UK', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const value = listing();
    const rendered: string[] = [];

    for (const locale of ['en', 'uk'] as const) {
      const renderer = await renderPanel(value, locale);
      const text = textContent(renderer.toJSON()).join(' ');
      rendered.push(text);
      for (const key of [
        'calc.partial_listing_warning',
        'calc.terminal.iteration_limit', 'calc.terminal.time_budget_skipped', 'calc.terminal.time_budget_hit',
        'calc.terminal.cell_budget', 'calc.terminal.resolution_limited',
        'calc.pick.saturated', 'calc.pick.ambiguous', 'calc.s_pick',
        'calc.reason.session_gap', 'calc.reason.timestamp_non_monotonic',
        'calc.reason.duplicate_sensor_position', 'calc.reason.reflected_transform',
      ] as const) {
        expect(text).toContain(translate(locale, key));
      }
      expect(text).toContain(translate(locale, 'calc.s_pick_sample', { index: formatNumber(locale, 150) }));
      expect(text).toContain(translate(locale, 'calc.s_pick_uncertainty', { value: formatNumber(locale, 18) }));
      expect(text).toContain(translate(locale, 'calc.session_epoch', {
        value: formatNumber(locale, 1700000000123456),
      }));
      expect(text).toContain(`${formatNumber(locale, 0.15, { minimumFractionDigits: 3, maximumFractionDigits: 3 })} s`);
      expect(text).not.toContain('iteration_limit');
      expect(text).not.toContain('session_gap');
      dispose(renderer);
    }

    expect(rendered[0]).not.toBe(rendered[1]);
    expect(value.runs.map((entry) => entry.terminal_status)).toEqual([
      'iteration_limit', 'time_budget_skipped', 'time_budget_hit', 'cell_budget', 'resolution_limited',
    ]);
    expect(value.picks[0].channels.map((channel) => channel.status)).toEqual([
      'rejected_saturated', 'rejected_ambiguous',
    ]);
  });

  it.each(['en', 'uk'] as const)('keeps the partial warning visible when every listing is empty in %s', async (locale) => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const value = listing();
    value.runs = [];
    value.picks = [];
    const renderer = await renderPanel(value, locale);
    const text = textContent(renderer.toJSON()).join(' ');
    expect(text).toContain(translate(locale, 'calc.partial_listing_warning'));
    expect(text.toLowerCase()).not.toContain('no picks');
    dispose(renderer);
  });
});
