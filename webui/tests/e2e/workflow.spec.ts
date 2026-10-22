import { test, expect, catalog } from './fixtures/device';
import { gate, scenarioScreenshot } from './fixtures/scenarios';

test('one attached remote can enter the permitted initialization stage', async ({ page, api }) => {
  api.status.pods = [api.status.pods[0]];
  api.status.system.ready_pods = 1;
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', assigned_remotes: 1, allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  api.once('POST /api/control', async route => {
    expect(route.request().postDataJSON()).toEqual({ action: 'workflow_command', command: 'GO_CALIBRATION', generation: 1 });
    Object.assign(api.status.workflow!, { step: 'CALIB_PREP', generation: 2, allowed_actions: ['SENSORS_PLACED', 'BACK'] });
    await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
  });
  await page.goto('/');
  await page.locator('[data-command="GO_CALIBRATION"]').click();
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.calib_prep'], exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(1);
  await scenarioScreenshot(page, 'one-remote-init');
});

for (const reason of ['stale', 'not_allowed'] as const) {
  test(`409 ${reason} refreshes the authoritative stage without replay`, async ({ page, api }) => {
    api.expectedHttpErrors.add('/api/control');
    Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
    api.once('POST /api/control', async route => {
      Object.assign(api.status.workflow!, { step: 'OBJECT', generation: 9, allowed_actions: ['START_MEASUREMENT', 'BACK'] });
      await route.fulfill({ status: 409, json: { ok: false, reason } });
    });
    await page.goto('/');
    await page.locator('[data-command="GO_CALIBRATION"]').click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.object'], exact: true })).toBeVisible();
    await expect(page.getByRole('status')).toContainText(catalog.en['workflow.state_changed']);
    expect(api.controls).toHaveLength(1);
    await scenarioScreenshot(page, `workflow-${reason}`);
  });
}

test('HTTP command failure leaves the stage intact and a manual retry can recover', async ({ page, api }) => {
  api.expectedHttpErrors.add('/api/control');
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  api.once('POST /api/control', async route => { await route.fulfill({ status: 500, json: { error: 'init_failed' } }); });
  await page.goto('/');
  await page.locator('[data-command="GO_CALIBRATION"]').click();
  await expect(page.getByRole('alert')).toContainText('500');
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.sensor_check'], exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(1);
  api.once('POST /api/control', async route => {
    Object.assign(api.status.workflow!, { step: 'CALIB_PREP', generation: 2, allowed_actions: ['SENSORS_PLACED', 'BACK'] });
    await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
  });
  await page.locator('[data-command="GO_CALIBRATION"]').click();
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.calib_prep'], exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(2);
});

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    test('record, stop, record again and arm/confirm deletion with current generations', async ({ page, api }) => {
      const t = catalog[locale];
      const responses = [
        ['START_RECORDING', 'RECORDING', ['STOP_RECORDING']],
        ['STOP_RECORDING', 'STOPPED', ['RECORD_AGAIN', 'DELETE_RECORDING', 'BACK']],
        ['RECORD_AGAIN', 'RECORD_READY', ['START_RECORDING', 'BACK']],
        ['START_RECORDING', 'RECORDING', ['STOP_RECORDING']],
        ['STOP_RECORDING', 'STOPPED', ['DELETE_RECORDING', 'BACK']],
        ['DELETE_RECORDING', 'RECORD_READY', ['START_RECORDING', 'BACK']],
      ] as const;
      let index = 0;
      Object.assign(api.status.workflow!, { step: 'RECORD_READY', allowed_actions: ['START_RECORDING', 'BACK'] });
      api.overrides.set('POST /api/control', async route => {
        const body = route.request().postDataJSON();
        const [command, step, allowed] = responses[index];
        expect(body).toEqual({ action: 'workflow_command', command, generation: index + 1 });
        index += 1;
        Object.assign(api.status.workflow!, { step, allowed_actions: [...allowed], generation: index + 1 });
        await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
      });
      await page.goto('/');
      for (const [command, step] of responses) {
        const button = page.locator(`[data-command="${command}"]`);
        if (command === 'DELETE_RECORDING') {
          const before = api.controls.length;
          await button.click();
          await expect(page.getByText(t['workflow.delete_confirm.prompt'], { exact: true })).toBeVisible();
          expect(api.controls).toHaveLength(before);
          await scenarioScreenshot(page, `${locale}-armed-delete`);
        }
        await button.click();
        await expect(page.getByRole('heading', { name: t[`workflow.title.${step.toLowerCase()}` as keyof typeof t], exact: true })).toBeVisible();
      }
      expect(api.controls).toHaveLength(6);
    });
  });
}

test('pending command disables its actions and navigating away does not duplicate it', async ({ page, api }) => {
  const pending = gate();
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  api.once('POST /api/control', async route => {
    await pending.promise;
    Object.assign(api.status.workflow!, { step: 'CALIB_PREP', generation: 2, allowed_actions: ['SENSORS_PLACED', 'BACK'] });
    await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
  });
  try {
    await page.goto('/');
    await page.locator('[data-command="GO_CALIBRATION"]').click();
    await expect(page.locator('[data-command="GO_CALIBRATION"]')).toBeDisabled();
    await page.getByRole('navigation').getByRole('button', { name: catalog.en['nav.home'], exact: true }).click();
    pending.release();
    await expect(page.getByRole('heading', { name: catalog.en['dashboard.title'], exact: true })).toBeVisible();
    await expect.poll(() => api.controls.length).toBe(1);
    await page.getByRole('navigation').getByRole('button', { name: catalog.en['workflow.nav'], exact: true }).click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.calib_prep'], exact: true })).toBeVisible();
    expect(api.controls).toHaveLength(1);
  } finally { pending.release(); }
});

for (const permission of ['granted', 'denied'] as const) {
  test(`phone GPS ${permission} produces one generation-scoped completion`, async ({ page, api, context }) => {
    Object.assign(api.status.workflow!, { step: 'GPS_CHOICE', allowed_actions: ['REQUEST_GPS', 'SKIP_GPS'] });
    if (permission === 'granted') {
      await context.grantPermissions(['geolocation']);
      await context.setGeolocation({ latitude: 50.45, longitude: 30.52, accuracy: 4 });
    } else await page.addInitScript(() => {
      Object.defineProperty(navigator, 'geolocation', { value: {
        getCurrentPosition: (_ok: unknown, fail: (e: { code: number; message: string }) => void) => fail({ code: 1, message: 'Permission denied by test' }),
      } });
    });
    api.overrides.set('POST /api/control', async route => {
      const body = route.request().postDataJSON();
      if (body.command === 'REQUEST_GPS') {
        expect(body.generation).toBe(1);
        Object.assign(api.status.workflow!, { step: 'GPS_PENDING', gps_status: 'pending', generation: 2, allowed_actions: ['BACK'] });
      } else {
        expect(body.command).toBe(permission === 'granted' ? 'GPS_ACCEPT' : 'GPS_CAPTURE_FAILED');
        expect(body.generation).toBe(2);
        if (permission === 'granted') expect(body).toMatchObject({ latitude: 50.45, longitude: 30.52, accuracy_m: 4 });
        Object.assign(api.status.workflow!, { step: 'LAYOUT_READY', gps_status: permission === 'granted' ? 'accepted' : 'failed', generation: 3, allowed_actions: ['BACK'] });
      }
      await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
    });
    await page.goto('/');
    await page.locator('[data-command="REQUEST_GPS"]').click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.layout_ready'], exact: true })).toBeVisible();
    expect(api.controls.map(c => c.command)).toEqual(['REQUEST_GPS', permission === 'granted' ? 'GPS_ACCEPT' : 'GPS_CAPTURE_FAILED']);
    await scenarioScreenshot(page, `gps-${permission}`);
  });
}
