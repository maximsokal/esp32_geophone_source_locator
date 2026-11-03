import { beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import type { DeviceStatus, DeviceWorkflowStatus } from '../../src/domain/device';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import { rangingPairDiagnostics, rangingPairsSummary } from '../../src/features/workflow/workflowViewModel';
import { loadUkrainianCatalog, LocaleProvider, translate } from '../../src/i18n';

beforeAll(loadUkrainianCatalog);

function statusFor(step: string): DeviceStatus {
  const workflow = {
    step, generation: 7, allowed_actions: [], sensor_sufficiency: 'CAPABLE_3D', assigned_remotes: 4, reason: null, progress: null,
    calibration: { generation: 1, phase: 'pending', per_node: [], cs: { pairs_done: 0, pairs_total: 0, max_spread_mm: null }, result: null, reasons: [] },
    placement: { per_node: [], all_stable: false, max_range_mm: 100, min_samples: 8 }, object_id: 'OBJ_0001', session_id: 'S000001',
    has_scan: false, calculation_class: 'none', gps_status: 'none', layout_result: null, layout_reason: '', scan_choice: 'none',
    scan_upload_status: 'none',
  } as unknown as DeviceWorkflowStatus;
  return { workflow, pods: [] } as unknown as DeviceStatus;
}

function panelHtml(status: DeviceStatus): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(WorkflowPanel, {
    status, objectName: 'Quarry', sendCommand: vi.fn(), onRefreshStatus: async () => true, onWorkflowUpdate: () => undefined,
    onOpenScanner: () => undefined,
  })));
}

describe('a ranging pair without a distance', () => {
/*
 * Bench 2026-11-03 (ESP32 log 16:27): MAIN-G4 had no distance and the page said only "Not measured", although MAIN->G4 had produced
 * 8.8 m (3 results, 1 above the confidence limit) and G4->MAIN 6.4 m (10 results, 9 accepted). The numbers and the cause are shown,
 * and "5 valid pairs of 6" is told apart from "nothing measured".
 */
const BENCH_PAIRS = [
  { node_a: 0, node_b: 1, state: 'valid', distance_mm: 4977, spread_mm: 234, confidence: 877, valid_samples: 15, attempts: 18,
    single_direction: false, direction_disagreement_mm: 0, cause: 'none', diag_ab_mm: 4977, diag_ba_mm: 4977,
    diag_ab_samples: 6, diag_ba_samples: 9, diag_ab_accepted: 6, diag_ba_accepted: 9 },
  { node_a: 0, node_b: 4, state: 'retry_exhausted', distance_mm: 0, spread_mm: 0, confidence: 0, valid_samples: 0, attempts: 13,
    single_direction: false, direction_disagreement_mm: 0, cause: 'low_confidence', diag_ab_mm: 8845, diag_ba_mm: 6421,
    diag_ab_samples: 3, diag_ba_samples: 10, diag_ab_accepted: 1, diag_ba_accepted: 9 },
  { node_a: 3, node_b: 4, state: 'valid', distance_mm: 4099, spread_mm: 33, confidence: 996, valid_samples: 18, attempts: 18,
    single_direction: false, direction_disagreement_mm: 0, cause: 'none', diag_ab_mm: 4099, diag_ba_mm: 4099,
    diag_ab_samples: 9, diag_ba_samples: 9, diag_ab_accepted: 9, diag_ba_accepted: 9 },
];

it('keeps the numbers of a pair that has no distance, and the cause, in the view model', () => {
  expect(rangingPairDiagnostics(BENCH_PAIRS[0])).toBeNull();                       // a valid pair needs no explanation
  expect(rangingPairDiagnostics(BENCH_PAIRS[1])).toEqual({
    cause: 'low_confidence',
    forward: { mm: 8845, samples: 3, accepted: 1 },
    reverse: { mm: 6421, samples: 10, accepted: 9 },
  });
  // One direction with nothing at all, an unknown cause from a newer firmware, and an old device without the fields.
  expect(rangingPairDiagnostics({ state: 'retry_exhausted', cause: 'no_measurement', diag_ab_samples: 0, diag_ba_mm: 3900, diag_ba_samples: 9, diag_ba_accepted: 9 }))
    .toEqual({ cause: 'no_measurement', forward: null, reverse: { mm: 3900, samples: 9, accepted: 9 } });
  expect(rangingPairDiagnostics({ state: 'missing', cause: 'something_new' })).toBeNull();
  expect(rangingPairDiagnostics({ state: 'retry_exhausted' })).toBeNull();
  expect(rangingPairDiagnostics({ state: 'retry_exhausted', diag_ab_mm: Number.NaN, diag_ab_samples: 3 })).toBeNull();
  expect(rangingPairsSummary(BENCH_PAIRS)).toEqual({ valid: 2, total: 3, failed: [{ a: 0, b: 4, cause: 'low_confidence' }] });
  expect(rangingPairsSummary([{ node_a: 0, node_b: 1, state: 'pending' }, { node_a: 0, node_b: 2, state: 'retry_exhausted', cause: 'no_measurement' }]))
    .toEqual({ valid: 0, total: 2, failed: [{ a: 0, b: 2, cause: 'no_measurement' }] });
});

it.each(['en', 'uk'] as const)('shows the failed pair with both directions, the cause and the pair summary in %s', async (locale) => {
  await loadUkrainianCatalog();
  const status = statusFor('RANGING_RESULT');
  (status as unknown as { ranging: unknown }).ranging = {
    available: true,
    progress: { complete: true, success: false, partial: true, pairs_done: 3, pairs_total: 3, pairs: BENCH_PAIRS,
      failure_reason: 'pair_failed:0-4' },
  };
  const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale },
    createElement(WorkflowPanel, { status, objectName: 'Quarry', sendCommand: vi.fn(), onRefreshStatus: async () => true,
      onWorkflowUpdate: () => undefined, onOpenScanner: () => undefined })));
  expect(html).toContain('data-valid-pairs="2"');
  expect(html).toContain('data-total-pairs="3"');
  expect(html).toContain(translate(locale, 'workflow.ranging.pairs_valid', { valid: '2', total: '3' }));
  expect(html).toContain('data-pair-cause="low_confidence"');
  expect(html).toContain(translate(locale, 'workflow.ranging.pair_cause.low_confidence'));
  // 8.8 m = 885 cm of 3 results with 1 accepted, 6.4 m = 642 cm of 10 with 9 accepted: both directions are on the page.
  const side = (distance: string, accepted: string, samples: string) => translate(locale, 'workflow.ranging.pair_diag_side', { distance, accepted, samples });
  expect(html).toContain(side(`885 ${locale === 'uk' ? 'см' : 'cm'}`, '1', '3'));
  expect(html).toContain(side(`642 ${locale === 'uk' ? 'см' : 'cm'}`, '9', '10'));
  // The failed pair still has no published distance; the valid ones are untouched and carry no explanation.
  expect(html).toContain(`${translate(locale, 'workflow.ranging.pair_distance', { a: '0', b: '4' })}: ${translate(locale, 'workflow.ranging.pair_unavailable')}`);
  expect(html.match(/data-pair-cause=/g)).toHaveLength(1);
});

it('says nothing extra for a cycle where every pair is valid or the device does not report the diagnostics yet', () => {
  const status = statusFor('RANGING_RESULT');
  (status as unknown as { ranging: unknown }).ranging = {
    available: true,
    progress: { complete: true, success: true, pairs_done: 1, pairs_total: 1, pairs: [BENCH_PAIRS[0]] },
  };
  expect(panelHtml(status)).not.toContain('data-valid-pairs');
  const old = statusFor('RANGING_RESULT');
  (old as unknown as { ranging: unknown }).ranging = {
    available: true,
    progress: { complete: true, success: false, partial: true, pairs_done: 1, pairs_total: 1, pairs: [
      { node_a: 0, node_b: 4, state: 'retry_exhausted', distance_mm: 0, spread_mm: 0, confidence: 0, valid_samples: 0, attempts: 13,
        single_direction: false, direction_disagreement_mm: 0 }] },
  };
  const html = panelHtml(old);
  expect(html).toContain('Not measured');
  expect(html).not.toContain('data-pair-cause');
  expect(html).not.toContain('Measured but not used');
});

it('shows partial measurements even when the remaining geometry is usable', () => {
  const status = statusFor('RANGING_RESULT');
  (status as unknown as { ranging: unknown }).ranging = {
    available: true,
    progress: { complete: true, success: true, partial: true, pairs_done: 3, pairs_total: 3, pairs: BENCH_PAIRS },
  };
  const html = panelHtml(status);
  expect(html).toContain(translate('en', 'workflow.ranging.partial'));
  expect(html).not.toContain(translate('en', 'workflow.ranging.success'));
  expect(html).toContain('data-valid-pairs="2"');
});
});
