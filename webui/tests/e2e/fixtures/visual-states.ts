import { expect, type Page } from '@playwright/test';
import type { WorkflowStep } from '../../../src/domain/device';
import { catalog } from './device';
import type { DeviceApi } from './api';
import { openSection, type Locale, type Section } from '../helpers/layout';
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
  variant?: 'loading' | 'empty' | 'warning' | 'error' | 'disconnected' | 'confirmation' | 'long';
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
  if (state.step === 'RANGING' || state.step === 'RANGING_RESULT') {
    const complete = state.step === 'RANGING_RESULT';
    Object.assign(api.status.ranging!.progress!, { active: !complete, complete, success: complete, pairs_done: complete ? 3 : 1, pairs_total: 3 });
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
    const done = state.step === 'RANGING_RESULT' ? 3 : 1;
    const percent = state.step === 'RANGING_RESULT' ? 100 : 33;
    await expect(page.getByText(new RegExp(String.raw`${done} / 3 \(${percent}\s?%\)`))).toBeVisible();
    await expect(page.getByRole('progressbar', { name: t['workflow.ranging.pairs'] }))
      .toHaveAttribute('value', String(done / 3 * 100));
  }
  await expect(page.getByText(t['workflow.result.loading'], { exact: true })).toHaveCount(0);
  await page.evaluate(() => window.scrollTo(0, 0));
}
