import { test, expect, catalog } from './fixtures/device';
import { workflowActions } from './fixtures/visual-states';
import type { WorkflowStep } from '../../src/domain/device';

/**
 * The device's HTTPS server loses connections: a TLS handshake that cannot get memory ends in a reset, and a request that
 * reaches the device may or may not have been carried out (board log 2026-11-15 22:06: 18 of 26 connections failed to
 * handshake). The operator clicks through calibration, placement, GPS and ranging; at every button the first request is
 * lost. The page must say so, must not move on by itself, and the same button must work when the link is back.
 */
interface Move { command: string; from: WorkflowStep; to: WorkflowStep; actions?: string[] }

const PIPELINE: Move[] = [
  { command: 'START_MEASUREMENT', from: 'OBJECT', to: 'SENSOR_CHECK' },
  { command: 'GO_CALIBRATION', from: 'SENSOR_CHECK', to: 'CALIB_PREP' },
  { command: 'SENSORS_PLACED', from: 'CALIB_PREP', to: 'CALIBRATING' },
  { command: 'PLACE_SENSORS', from: 'CALIB_RESULT', to: 'PLACEMENT' },
  { command: 'PLACEMENT_CONFIRMED', from: 'PLACEMENT', to: 'GPS_CHOICE' },
  { command: 'GPS_SKIP', from: 'GPS_CHOICE', to: 'LAYOUT_READY' },
  { command: 'CALCULATE_POSITIONS_KEEP_WIFI', from: 'LAYOUT_READY', to: 'RANGING' },
];

for (const locale of ['en', 'uk'] as const) {
  test.describe(`lost requests during the operator's pipeline ${locale}`, () => {
    test.use({ appLocale: locale });
    for (const [failIndex, failing] of PIPELINE.entries()) {
      test(`${failing.command}: the first request is lost, the page stays put and the retry moves on`, async ({ page, api }) => {
        test.setTimeout(90_000);
        const t = catalog[locale];
        const workflow = api.status.workflow!;
        const nodes = ['MAIN', 'G1', 'G2', 'G4'];
        api.status.pods = [1, 2, 4].map((id) => ({ ...structuredClone(api.status.pods[0]), id, state: 'READY' }));
        api.status.system.ready_pods = 3;
        workflow.assigned_remotes = 3;
        Object.assign(workflow.placement, { all_stable: true, per_node: nodes.map((node) => ({
          node, vertical: true, height_rel_main_mm: 0, altitude_asl_m: 12.5, stable: true, reason: 'stable', samples: 4, range_mm: 3,
        })) });
        const enter = (step: WorkflowStep, actions = workflowActions[step]) => {
          Object.assign(workflow, { step, generation: workflow.generation + 1, allowed_actions: [...actions] });
        };
        enter('OBJECT');
        // The lost request is the point of the test: the browser reports it as a failed resource load.
        api.expectedHttpErrors.add('/api/control');
        api.expectedConsoleErrors.push('Failed to load resource: net::ERR_CONNECTION_RESET');

        let lose: string | null = null;
        const applied: string[] = [];
        let lost = 0;
        api.overrides.set('POST /api/control', async (route) => {
          const body = route.request().postDataJSON();
          if (body.command === lose) {
            lose = null;
            lost += 1;
            // The request never reaches the device: the handshake failed.
            await route.abort('connectionreset');
            return;
          }
          expect(body.generation, `${body.command} was sent with a stale generation`).toBe(workflow.generation);
          expect(workflow.allowed_actions, `${body.command} must be allowed at ${workflow.step}`).toContain(body.command);
          applied.push(body.command);
          const move = PIPELINE.find((m) => m.command === body.command)!;
          if (body.command === 'CALCULATE_POSITIONS_KEEP_WIFI') {
            Object.assign(api.status.ranging!.progress!, { active: true, complete: false, success: false, pairs_done: 0, pairs_total: 12 });
          }
          enter(move.to, move.actions ?? workflowActions[move.to]);
          await route.fulfill({ json: { ok: true, workflow } });
        });

        const titleOf = (step: WorkflowStep) => t[`workflow.title.${step.toLowerCase()}` as keyof typeof t];
        await page.goto('/');
        await page.getByRole('navigation').getByRole('button', { name: t['workflow.nav'], exact: true }).click();

        for (const [index, move] of PIPELINE.entries()) {
          if (move.from === 'CALIB_RESULT') {
            // Calibration runs on the device; it finishes by itself.
            Object.assign(workflow.calibration, { phase: 'complete', result: true,
              per_node: nodes.map((node) => ({ node, height: { state: 'valid', relative_z_mm: 0 },
                imu: { state: 'calibrated', gyro_bias_mdps: [0, 0, 0], gravity_ref_mg: [0, 0, 1000] }, cs_pairs_ok: true })),
              cs: { pairs_done: 12, pairs_total: 12, max_spread_mm: 5 } });
            enter('CALIB_RESULT', ['PLACE_SENSORS', 'BACK']);
          }
          await expect(page.locator('.workflow-page h2')).toHaveText(titleOf(move.from));
          const button = page.locator(`[data-command="${move.command}"]`);
          if (index === failIndex) {
            lose = move.command;
            await button.click();
            await expect.poll(() => lost, { message: 'the request was supposed to be lost' }).toBe(1);
            // Not applied, and not pretended to be: still on the same step, with the reason in front of the operator.
            await expect(page.locator('.workflow-page h2')).toHaveText(titleOf(move.from));
            await expect(page.getByRole('alert').first()).toBeVisible();
            expect(applied).not.toContain(move.command);
            // The link is back: the same button, no reload.
            await expect(button).toBeEnabled();
          }
          await button.click();
          await expect(page.locator('.workflow-page h2')).toHaveText(titleOf(move.to));
          expect(applied.filter((command) => command === move.command), 'a command must be carried out once').toHaveLength(1);
          if (index === failIndex) break;
        }
      });
    }
  });
}
