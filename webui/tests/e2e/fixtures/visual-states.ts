import { expect, type Page } from '@playwright/test';
import type { WorkflowStep } from '../../../src/domain/device';
import { catalog } from './device';
import type { DeviceApi } from './api';
import { openSection, type Locale, type Section } from '../helpers/layout';
import { installSavedScan, openSavedSessionScan } from './saved-scan';
import actions from '../../fixtures/max460_workflow_allowed_actions.json' with { type: 'json' };

export const workflowActions: Record<WorkflowStep, string[]> = {
  IDLE: [], OBJECT: ['START_MEASUREMENT', 'BACK'], SENSOR_CHECK: ['GO_CALIBRATION', 'BACK'],
  CALIB_PREP: ['SENSORS_PLACED', 'BACK'], CALIBRATING: ['BACK'], CALIB_RESULT: ['RETRY', 'BACK'],
  PLACEMENT: actions.PLACEMENT, GPS_CHOICE: actions.GPS_CHOICE, GPS_PENDING: actions.GPS_PENDING,
  LAYOUT_READY: actions.LAYOUT_READY, RANGING: ['BACK'], RANGING_RESULT: ['RETRY', 'BACK'],
  SCAN_CHOICE: actions.SCAN_CHOICE, SCANNING: actions.SCANNING,
  RECORD_READY: ['START_RECORDING', 'MOVE_SENSORS', 'BACK'], RECORDING: ['STOP_RECORDING'],
  STOPPED: ['DELETE_RECORDING', 'RECORD_AGAIN', 'MOVE_SENSORS', 'BACK'], CALCULATING: ['BACK'], RESULT: ['MOVE_SENSORS', 'BACK'],
};

export interface VisualState {
  id: string;
  locale: Locale;
  tier: 'critical' | 'extended';
  section: Section;
  width: number;
  height: number;
  step?: WorkflowStep;
  variant?: 'loading' | 'empty' | 'warning' | 'error' | 'disconnected' | 'confirmation' | 'long' | 'wifi-pending' | 'wifi-off' | 'wifi-back' | 'scan-refinement' | 'pair-failed' | 'partial-usable';
}

export const visualStates: VisualState[] = [];
for (const locale of ['en', 'uk'] as const) {
  const add = (name: string, section: Section, variant?: VisualState['variant'], width = 393,
    height = 852, tier: VisualState['tier'] = 'critical', step?: WorkflowStep) => {
    visualStates.push({ id: `${locale}-${name}-${width}x${height}`, locale, section, variant, width, height, tier, step });
  };
  for (const section of ['home', 'objects', 'sensors', 'scanner'] as const) add(section, section);
  for (const step of Object.keys(workflowActions) as WorkflowStep[]) {
    add(`workflow-${step.toLowerCase().replaceAll('_', '-')}`, 'workflow', undefined, 393, 852, 'critical', step);
  }
  add('home-loading', 'home', 'loading');
  add('objects-empty', 'objects', 'empty');
  add('sensors-warning', 'sensors', 'warning');
  add('home-error', 'home', 'error');
  add('sensors-disconnected', 'sensors', 'disconnected');
  add('workflow-confirmation', 'workflow', 'confirmation', 393, 852, 'critical', 'STOPPED');
  /* The Wi-Fi radio goes off while the nodes range and comes back by itself: what the page says at each moment. */
  add('workflow-ranging-wifi-pending', 'workflow', 'wifi-pending', 393, 852, 'critical', 'RANGING');
  add('workflow-ranging-wifi-off', 'workflow', 'wifi-off', 393, 852, 'critical', 'RANGING');
  add('workflow-ranging-result-wifi-back', 'workflow', 'wifi-back', 393, 852, 'critical', 'RANGING_RESULT');
  /* A cycle where five pairs are valid and one failed: the numbers of both directions and the cause are shown (bench 2026-11-03). */
  add('workflow-ranging-result-pair-failed', 'workflow', 'pair-failed', 393, 852, 'critical', 'RANGING_RESULT');
  /* The scan information screen after the devices were scanned (the WebGL canvas is hidden: canvas pixels are separate coverage). */
  add('scan-distance-refinement', 'objects', 'scan-refinement', 393, 852, 'critical');
  add('objects-long', 'objects', 'long');
  add('objects-long', 'objects', 'long', 320, 568);
  add('sensors-disconnected', 'sensors', 'disconnected', 320, 568);
  add('scanner', 'scanner', undefined, 320, 568);
  for (const [width, height] of [[360, 800], [412, 915], [430, 932], [844, 390], [768, 1024]]) {
    add('home', 'home', undefined, width, height, 'extended');
    add('sensors-warning', 'sensors', 'warning', width, height, 'extended');
  }
}

export async function prepareVisualState(page: Page, api: DeviceApi, state: VisualState) {
  const t = catalog[state.locale];
  if (state.step) Object.assign(api.status.workflow!, { step: state.step, allowed_actions: workflowActions[state.step] });
  // Native indeterminate progress animates outside CSS animations; visual normal states use known API progress.
  if (state.step === 'CALCULATING') api.status.workflow!.progress = 37;
  if ((state.step === 'RANGING' || state.step === 'RANGING_RESULT') && state.variant !== 'partial-usable') {
    const complete = state.step === 'RANGING_RESULT';
    Object.assign(api.status.ranging!.progress!, { active: !complete, complete, success: complete, pairs_done: complete ? 3 : 1, pairs_total: 3 });
  }
  if (state.variant === 'wifi-pending' || state.variant === 'wifi-off' || state.variant === 'wifi-back') {
    api.status.ranging!.radio = state.variant === 'wifi-pending' ? 'pending' : state.variant === 'wifi-off' ? 'off' : 'resuming';
  }
  if (state.variant === 'pair-failed') {
    const pair = (a: number, b: number, distance: number, ab: number, ba: number, samples: [number, number], accepted: [number, number],
      failed = false) => ({ node_a: a, node_b: b, state: failed ? 'retry_exhausted' : 'valid', distance_mm: distance, spread_mm: 40,
      confidence: failed ? 0 : 900, valid_samples: failed ? 0 : 18, attempts: failed ? 13 : 18, single_direction: false,
      direction_disagreement_mm: 0, cause: failed ? 'low_confidence' : 'none', diag_ab_mm: ab, diag_ba_mm: ba, diag_ab_samples: samples[0],
      diag_ba_samples: samples[1], diag_ab_accepted: accepted[0], diag_ba_accepted: accepted[1] });
    Object.assign(api.status.ranging!.progress!, { complete: true, success: false, partial: true, pairs_done: 3, pairs_total: 3,
      failure_reason: 'pair_failed:0-4',
      pairs: [pair(0, 1, 4977, 4977, 4977, [6, 9], [6, 9]), pair(0, 4, 0, 8845, 6421, [3, 10], [1, 9], true), pair(3, 4, 4099, 4099, 4099, [9, 9], [9, 9])] });
  }
  if (state.variant === 'partial-usable') {
    const points = [[0, 0], [3000, 0], [0, 3000], [3000, 3000], [1500, 1000]];
    const pairs = points.flatMap((a, i) => points.slice(i + 1).map((b, offset) => {
      const j = i + offset + 1;
      const failed = i === 3 && j === 4;
      return { node_a: i, node_b: j, state: failed ? 'retry_exhausted' : 'valid',
        distance_mm: failed ? 0 : Math.round(Math.hypot(a[0] - b[0], a[1] - b[1])),
        spread_mm: 20, confidence: failed ? 0 : 900, valid_samples: failed ? 0 : 18,
        attempts: 18, single_direction: false, direction_disagreement_mm: 0 };
    }));
    Object.assign(api.status.ranging!.progress!, { complete: true, success: true, partial: true,
      pairs_done: 20, pairs_total: 20, failure_reason: '', pairs,
      geometry: { nodes: points.map((p, node_id) => ({ node_id, solved: true, x_mm: p[0], y_mm: p[1],
        z_mm: 0, node_residual_mm: 1, uncertainty_mm: 50 })), residual_mm: 1, max_residual_mm: 1 } });
    api.status.workflow!.allowed_actions = ['CONFIRM', 'SET_DISTANCE', 'BACK'];
  }
  if (state.variant === 'scan-refinement') {
    installSavedScan(api, true);
    await page.setViewportSize({ width: state.width, height: state.height });
    await page.goto('/');
    await expect(page.locator('.device-dot')).toHaveClass(/online/);
    await expect(page.locator('.locale-picker select')).toHaveAttribute('aria-busy', 'false');
    await openSavedSessionScan(page, state.locale);
    const card = page.locator('.scan-refinement');
    await expect(card).toHaveAttribute('data-refinement-status', 'ok', { timeout: 30_000 });
    await expect(card.getByText(t['scan.refine.title'], { exact: true })).toBeVisible();
    await page.addStyleTag({ content: '.scan-webgl-canvas { visibility: hidden !important; }' });
    await page.evaluate(() => document.fonts.ready);
    await page.evaluate(() => window.scrollTo(0, 0));
    return;
  }
  if (state.variant === 'loading') api.holdStatus();
  if (state.variant === 'error') api.statusFailure = 'http';
  if (state.variant === 'empty') {
    Object.assign(api.status.object_store!, { objects: [], active_id: null });
    Object.assign(api.status.workflow!, { step: 'IDLE', allowed_actions: [], object_id: null, session_id: null });
  }
  if (state.variant === 'long') {
    api.status.object_store!.objects[0].name = state.locale === 'en' ? 'LongQuarryNameWithoutBreaks123456' : 'ДовгаНазваОбєктаБезПробілів123456';
  }
  if (state.variant === 'warning') {
    Object.assign(api.status.power.channels![0], { stale: true, age_ms: 999999999 });
    api.status.pods[0].state = 'WARNING';
  }
  if (state.variant === 'disconnected') {
    api.status.pods = [];
    api.status.system.ready_pods = 0;
    Object.assign(api.status.workflow!, { assigned_remotes: 0, sensor_sufficiency: 'INSUFFICIENT' });
  }
  await page.setViewportSize({ width: state.width, height: state.height });
  await page.goto('/');
  if (state.step === 'SCANNING') {
    await page.getByRole('heading', { name: t['scan.search_volume_title'], exact: true }).waitFor();
  } else {
    if (state.variant === 'error') {
      await expect(page.getByRole('alert')).toContainText('503');
    } else if (state.variant === 'loading') {
      await expect(page.locator('.device-dot')).toHaveText(t['console.api_connecting']);
    } else {
      await expect(page.locator('.device-dot')).toHaveClass(/online/);
      await expect(page.locator('.brand-button small')).toHaveText(api.status.object_store?.objects[0]?.name ?? 'ESP32-S3');
    }
    await expect(page.locator('.locale-picker select')).toHaveAttribute('aria-busy', 'false');
    await openSection(page, state.section, state.locale);
  }
  if (state.variant === 'confirmation') {
    await page.getByRole('button', { name: t['workflow.action.delete_recording'], exact: true }).click();
    await page.getByText(t['workflow.delete_confirm.prompt'], { exact: true }).waitFor();
  }
  await page.evaluate(() => document.fonts.ready);
  if (state.step === 'RANGING' || state.step === 'RANGING_RESULT') {
    const total = state.variant === 'partial-usable' ? 20 : 3;
    const done = state.step === 'RANGING_RESULT' ? total : 1;
    const percent = state.step === 'RANGING_RESULT' ? 100 : 33;
    await expect(page.getByText(new RegExp(String.raw`${done} / ${total} \(${percent}\s?%\)`))).toBeVisible();
    await expect(page.getByRole('progressbar', { name: t['workflow.ranging.pairs'] }))
      .toHaveAttribute('value', String(done / total * 100));
  }
  if (state.variant === 'wifi-pending' || state.variant === 'wifi-off' || state.variant === 'wifi-back') {
    const key = state.variant === 'wifi-pending' ? 'workflow.ranging.wifi_pending' : state.variant === 'wifi-off' ? 'workflow.ranging.wifi_off' : 'workflow.ranging.wifi_back';
    await expect(page.locator('.workflow-ranging-wifi')).toHaveText(t[key]);
  }
  await expect(page.getByText(t['workflow.result.loading'], { exact: true })).toHaveCount(0);
  await page.evaluate(() => window.scrollTo(0, 0));
}
