import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

import frozenListing from '../../../tests/host/data/max132_e2e_runs.golden.json';

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

// This panel test checks localization output without mounting its browser-only Canvas scene.
vi.mock('../../src/features/objects/LocalizationOverlayScene', () => ({ default: () => null }));

import type { LocalizationRunListing } from '../../src/api/deviceApi';
import LocalizationCalculatePanel from '../../src/features/objects/LocalizationCalculatePanel';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';

beforeAll(async () => { await loadUkrainianCatalog(); });
afterEach(() => { vi.unstubAllGlobals(); vi.resetAllMocks(); });

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), {
    status: 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

function textContent(value: unknown): string[] {
  if (typeof value === 'string') return [value];
  if (Array.isArray(value)) return value.flatMap(textContent);
  if (typeof value === 'object' && value !== null && 'children' in value) {
    return textContent((value as { children: unknown }).children);
  }
  return [];
}

function machineSnapshot(listing: LocalizationRunListing): string {
  return JSON.stringify({
    runs: listing.runs.map((run) => ({
      method_name: run.method_name,
      kind: run.kind,
      solved: run.solved,
      rejection_reason: run.rejection_reason ?? null,
      source_position: run.source_position ?? null,
    })).sort((left, right) => left.method_name.localeCompare(right.method_name)),
    picks: listing.picks.map((record) => ({
      schema_version: record.schema_version,
      channels: record.channels.map((channel) => ({
        channel: channel.channel,
        participating: channel.participating,
        status: channel.status,
        method: channel.method,
        arrival_sample_index: channel.arrival_sample_index,
        arrival_time_s: channel.arrival_time_s,
        snr: channel.snr,
        uncertainty_samples: channel.uncertainty_samples,
      })),
    })),
  });
}

async function decodeThroughDeviceApi(golden: LocalizationRunListing): Promise<LocalizationRunListing> {
  const actualApi = await vi.importActual<typeof import('../../src/api/deviceApi')>('../../src/api/deviceApi');
  const fetchMock = vi.fn(async () => jsonResponse(golden));
  vi.stubGlobal('fetch', fetchMock);
  const decoded = await actualApi.fetchLocalizationRuns(golden.object_id, golden.session_id);
  expect(fetchMock).toHaveBeenCalledTimes(1);
  expect(String(fetchMock.mock.calls[0][0])).toContain('/api/localization/runs?object_id=');
  return decoded;
}

async function renderPanel(value: LocalizationRunListing, locale: I18nLocale): Promise<ReactTestRenderer> {
  api.fetchLocalizationRuns.mockResolvedValueOnce(value);
  api.fetchScanInfo.mockResolvedValueOnce({ present: false, bytes: 0 });
  api.getCalcSettings.mockResolvedValueOnce({ settings: null, presets: [], standardProcessing: {} });
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: locale }, createElement(LocalizationCalculatePanel, {
      objectId: value.object_id,
      sessionId: value.session_id,
      busy: false,
      onClose: () => undefined,
    })));
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
  return renderer;
}

function dispose(renderer: ReactTestRenderer): void {
  act(() => renderer.unmount());
}

describe('MAX-132 synthetic E2E localization listing', () => {
  it('decodes the C golden and renders methods, reasons, picks, and joint result in EN and UK', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const fixture = frozenListing as unknown as LocalizationRunListing;
    expect(frozenListing).not.toHaveProperty('pending_fixture_generation');

    const decodedByLocale: LocalizationRunListing[] = [];
    const renderedText: string[] = [];
    for (const locale of ['en', 'uk'] as const) {
      const decoded = await decodeThroughDeviceApi(fixture);
      decodedByLocale.push(decoded);

      const individualRuns = decoded.runs.filter((run) => run.kind === 'individual_method');
      const jointRuns = decoded.runs.filter((run) => run.kind === 'joint_fusion');
      expect(individualRuns).toHaveLength(20);
      expect(new Set(individualRuns.map((run) => run.method_name)).size).toBe(20);
      expect(jointRuns).toHaveLength(1);
      expect(jointRuns[0].method_name).toBe('joint_fusion');
      expect(individualRuns.filter((run) => !run.solved).length).toBeGreaterThan(0);
      for (const run of individualRuns.filter((entry) => !entry.solved)) {
        expect(run.rejection_reason?.trim().length, `${run.method_name} skip reason`).toBeGreaterThan(0);
      }
      expect(decoded.picks).toHaveLength(1);
      expect(decoded.picks[0].channels).toHaveLength(4);
      expect(decoded.picks[0].channels.every((channel) => channel.participating && channel.status === 'accepted')).toBe(true);
      expect(decoded.picks[0].channels.every((channel) => Number.isFinite(channel.arrival_time_s))).toBe(true);

      const renderer = await renderPanel(decoded, locale);
      const text = textContent(renderer.toJSON()).join(' ');
      renderedText.push(text);

      expect(text).toContain(translate(locale, 'calc.runs'));
      expect(text).toContain(translate(locale, 'calc.arrival_picks', { count: '1' }));
      expect(text).toContain(translate(locale, 'calc.pick.accepted'));
      expect(text).toContain(translate(locale, 'calc.joint_fusion'));
      for (const run of decoded.runs) {
        expect(text).toContain(run.method_name);
        if (!run.solved) expect(text).toContain(run.rejection_reason as string);
      }
      for (const channel of decoded.picks[0].channels) {
        expect(text).toContain(translate(locale, 'calc.channel', { number: String(channel.channel) }));
      }
      dispose(renderer);
    }

    expect(machineSnapshot(decodedByLocale[0])).toBe(machineSnapshot(decodedByLocale[1]));
    expect(renderedText[0]).not.toBe(renderedText[1]);
    expect(renderedText[0]).toContain(translate('en', 'calc.runs'));
    expect(renderedText[1]).toContain(translate('uk', 'calc.runs'));
  });
});
