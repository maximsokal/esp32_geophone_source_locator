import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({
  fetchLocalizationJob: vi.fn(),
  fetchLocalizationRuns: vi.fn(),
  fetchScanInfo: vi.fn(),
  sendControl: vi.fn(),
}));
vi.mock('../../src/api/deviceApi', () => api);

import type { LocalizationRunListing } from '../../src/api/deviceApi';
import LocalizationCalculatePanel from '../../src/features/objects/LocalizationCalculatePanel';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';

beforeAll(async () => { await loadUkrainianCatalog(); });
afterEach(() => { vi.unstubAllGlobals(); vi.resetAllMocks(); });

function listing(overrides: Partial<LocalizationRunListing> = {}): LocalizationRunListing {
  return {
    object_id: 'OBJ_0001', session_id: 'S000009',
    geometry: { available: true, pods: [{ pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 }] },
    cs_pairs: { available: false, pairs: [] }, picks: [], runs: [],
    ...overrides,
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

function fieldValue(text: string, label: string, expected: string): string {
  const escape = (value: string) => value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const match = text.match(new RegExp(`${escape(label)}(?::\\s*|\\s+)(${escape(expected)})(?:\\s|$)`));
  expect(match).not.toBeNull();
  return match?.[1] ?? '';
}

async function renderPanel(value: LocalizationRunListing, locale: I18nLocale): Promise<ReactTestRenderer> {
  api.fetchLocalizationRuns.mockResolvedValueOnce(value);
  api.fetchScanInfo.mockResolvedValueOnce({ present: false, bytes: 0 });
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

describe('localization geometry provenance', () => {
  it('shows every field and preserves IDs and generations in both locales', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const value = listing({
      geometry: {
        available: true,
        provenance: {
          solver: 'channel_solver', solver_version: 'v4.7', refined: true, refine_iterations: 5,
          topology_generation: 71, cs_membership_generation: 9, main_height_reference_generation: 11,
          setup_id: 'setup-7e', start_uptime_ms: 888001,
        },
        pods: [{
          pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0,
          source_sample_uptime_ms: 100000, esp_receipt_uptime_ms: 100040, source_age_ms_at_start: 40,
          height_reference_generation: 11,
        }],
      },
      cs_pairs: { available: true, pairs: [{
        node_a: 0, node_b: 1, valid: true, distance_mm: 1000, confidence: 950, attempts: 2, valid_samples: 2,
        state: 'PAIR_READY', last_failure: 'TIMEOUT', provider_quality_flags: 0, scheduler_quality_flags: 12,
      }] },
    });
    const texts: string[] = [];
    for (const locale of ['en', 'uk'] as const) {
      const renderer = await renderPanel(value, locale);
      const text = textContent(renderer.toJSON()).join(' ');
      texts.push(text);
      for (const key of [
        'calc.provenance', 'calc.provenance.solver', 'calc.provenance.solver_version', 'calc.provenance.refined',
        'calc.provenance.refine_iterations', 'calc.provenance.topology_generation',
        'calc.provenance.cs_membership_generation', 'calc.provenance.main_height_reference_generation',
        'calc.provenance.setup_id', 'calc.provenance.start_uptime_ms',
        'calc.provenance.source_sample_uptime_ms', 'calc.provenance.esp_receipt_uptime_ms',
        'calc.provenance.source_age_ms_at_start', 'calc.provenance.height_reference_generation',
        'calc.provenance.state', 'calc.provenance.last_failure',
        'calc.provenance.provider_quality_flags', 'calc.provenance.scheduler_quality_flags',
      ] as const) expect(text).toContain(translate(locale, key));
      dispose(renderer);
    }

    const labels = [
      'calc.provenance.solver', 'calc.provenance.solver_version', 'calc.provenance.refine_iterations',
      'calc.provenance.topology_generation', 'calc.provenance.cs_membership_generation',
      'calc.provenance.main_height_reference_generation', 'calc.provenance.setup_id',
      'calc.provenance.start_uptime_ms', 'calc.provenance.source_sample_uptime_ms',
      'calc.provenance.esp_receipt_uptime_ms', 'calc.provenance.source_age_ms_at_start',
      'calc.provenance.height_reference_generation', 'calc.provenance.state', 'calc.provenance.last_failure',
      'calc.provenance.provider_quality_flags', 'calc.provenance.scheduler_quality_flags',
    ] as const;
    const values = ['channel_solver', 'v4.7', '5', '71', '9', '11', 'setup-7e', '888001', '100000', '100040', '40', '11', 'PAIR_READY', 'TIMEOUT', '0', '12'];
    expect(texts.map((text, index) => labels.map((key, field) => fieldValue(
      text, translate(index === 0 ? 'en' : 'uk', key), values[field],
    ))))
      .toEqual([values, values]);
  });

  it('omits the provenance subsection for older firmware with no optional fields', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const value = listing({
      geometry: { available: true, pods: [{ pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 }] },
      cs_pairs: { available: true, pairs: [{ node_a: 0, node_b: 1, valid: true, distance_mm: 1000, confidence: 900, attempts: 1, valid_samples: 1 }] },
    });
    const renderer = await renderPanel(value, 'en');
    const text = textContent(renderer.toJSON()).join(' ');
    expect(text).not.toContain(translate('en', 'calc.provenance'));
    expect(text).not.toContain(translate('en', 'calc.provenance.state'));
    dispose(renderer);
  });

  it.each(['en', 'uk'] as const)('shows a null source age as unknown in %s', async (locale) => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const value = listing({
      geometry: { available: true, pods: [{
        pod: 1, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0, source_age_ms_at_start: null,
      }] },
    });
    const renderer = await renderPanel(value, locale);
    const text = textContent(renderer.toJSON()).join(' ');
    expect(text).toContain(translate(locale, 'calc.provenance.source_age_ms_at_start'));
    expect(text).toContain(translate(locale, 'report.unknown'));
    dispose(renderer);
  });
});
