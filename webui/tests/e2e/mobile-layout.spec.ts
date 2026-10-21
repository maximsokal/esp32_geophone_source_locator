import { test, expect, catalog } from './fixtures/device';
import { assertLayout, assertReachable, capture, contract, layoutIssues, openSection, sections } from './helpers/layout';
import { assertSemanticStyles } from './helpers/theme';
import { workflowViewModel } from '../../src/features/workflow/workflowViewModel';
import type { WorkflowStep } from '../../src/domain/device';
import actions from '../fixtures/max460_workflow_allowed_actions.json' with { type: 'json' };

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    test('reported Sensors spacing and scanner overlap', async ({ page }) => {
      await page.goto('/');
      const issues: string[] = [];
      for (const section of ['sensors', 'scanner'] as const) {
        await openSection(page, section, locale);
        await capture(page, `${locale}-reported-${section}`);
        issues.push(...await layoutIssues(page));
      }
      expect(issues).toEqual([]);
    });
    for (const [width, height] of contract.viewports) {
      test(`all menus ${width}x${height}`, async ({ page }) => {
        await page.setViewportSize({ width, height });
        await page.goto('/');
        for (const section of sections) {
          await test.step(section, async () => {
            await openSection(page, section, locale);
            await capture(page, `${locale}-${width}x${height}-${section}`);
            await assertLayout(page);
            await assertSemanticStyles(page);
            await assertReachable(page);
          });
        }
      });
    }
    test('200% text, long object name and resize', async ({ page, api }) => {
      api.status.object_store!.objects[0].name = locale === 'en' ? 'LongQuarryNameWithoutBreaks123456' : 'ДовгаНазваОбєктаБезПробілів123456';
      Object.assign(api.status.power.channels![0], { stale: true, age_ms: 999999999 });
      Object.assign(api.status.power.channels![1], { overcurrent: 'critical', current_a: 12345.678, overcurrent_latched: true });
      await page.setViewportSize({ width: 768, height: 1024 });
      await page.goto('/');
      await page.addStyleTag({ content: 'html { font-size: 200%; }' });
      await page.setViewportSize({ width: 320, height: 568 });
      for (const section of sections) {
        await openSection(page, section, locale);
        await capture(page, `${locale}-200percent-${section}`);
        await assertLayout(page);
        await assertSemanticStyles(page);
        await assertReachable(page);
      }
    });
    test('expanded Objects settings and scanner details', async ({ page }) => {
      await page.goto('/');
      for (const section of ['objects', 'scanner'] as const) {
        await openSection(page, section, locale);
        for (const details of await page.locator('details:not([open])').all()) {
          await details.locator('summary').first().click();
        }
        await capture(page, `${locale}-expanded-${section}`);
        await assertLayout(page);
        await assertReachable(page);
      }
    });
    const actionByStep: Record<WorkflowStep, string[]> = {
      IDLE: [], OBJECT: ['START_MEASUREMENT', 'BACK'], SENSOR_CHECK: ['GO_CALIBRATION', 'BACK'],
      CALIB_PREP: ['SENSORS_PLACED', 'BACK'], CALIBRATING: ['BACK'], CALIB_RESULT: ['RETRY', 'BACK'],
      PLACEMENT: actions.PLACEMENT, GPS_CHOICE: actions.GPS_CHOICE, GPS_PENDING: actions.GPS_PENDING,
      LAYOUT_READY: actions.LAYOUT_READY, RANGING: ['BACK'], RANGING_RESULT: ['RETRY', 'BACK'],
      SCAN_CHOICE: actions.SCAN_CHOICE, SCANNING: actions.SCANNING,
      RECORD_READY: ['START_RECORDING', 'MOVE_SENSORS', 'BACK'], RECORDING: ['STOP_RECORDING'],
      STOPPED: ['DELETE_RECORDING', 'RECORD_AGAIN', 'MOVE_SENSORS', 'BACK'], CALCULATING: ['BACK'], RESULT: ['MOVE_SENSORS', 'BACK'],
    };
    for (const step of Object.keys(actionByStep) as WorkflowStep[]) {
      test(`workflow ${step}`, async ({ page, api }) => {
        await page.setViewportSize({ width: 360, height: 800 });
        Object.assign(api.status.workflow!, { step, allowed_actions: actionByStep[step] });
        await page.goto('/');
        const title = workflowViewModel(api.status).titleKey;
        if (step === 'SCANNING') {
          await expect(page.getByRole('heading', { name: catalog[locale]['scan.search_volume_title'], exact: true })).toBeVisible();
        } else {
          await page.getByRole('navigation').getByRole('button', { name: catalog[locale]['workflow.nav'], exact: true }).click();
          await expect(page.locator('.workflow-page h2').first()).toHaveText(catalog[locale][title]);
        }
        expect(title).not.toBe('workflow.title.unknown');
        if (step === 'STOPPED') await expect(page.locator('.workflow-material-settings')).toBeVisible();
        await capture(page, `${locale}-workflow-${step}`);
        await assertLayout(page);
        await assertReachable(page);
      });
    }
  });
}
