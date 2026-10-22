import { test, expect, catalog } from './fixtures/device';
import { gate, scenarioScreenshot } from './fixtures/scenarios';

test.use({ manualClock: true });

for (const status of [400, 401, 403, 404, 409, 429, 500, 503]) {
  test(`HTTP ${status} is visible and an explicit refresh recovers`, async ({ page, api }) => {
    api.expectedHttpErrors.add('/api/status');
    api.overrides.set('GET /api/status', async route => {
      await route.fulfill({ status, json: { error: `injected_${status}` } });
    });
    await page.goto('/');
    await expect(page.getByRole('alert')).toContainText(String(status));
    await expect(page.locator('.device-dot')).toHaveClass(/offline/);
    if (status === 503 || status === 403) await scenarioScreenshot(page, `http-${status}`);
    api.overrides.delete('GET /api/status');
    await page.getByRole('button', { name: catalog.en['common.refresh'], exact: true }).click();
    await expect(page.locator('.device-dot')).toHaveClass(/online/);
    await expect(page.getByRole('alert')).toHaveCount(0);
  });
}

for (const [name, body] of [['truncated JSON', '{"system":'], ['empty body', ''],
  ['null body', 'null'], ['partial required status', '{"system":{"state":"READY"}}']]) {
  test(`${name} cannot report successful status; valid retry recovers`, async ({ page, api }) => {
    api.once('GET /api/status', async route => { await route.fulfill({ contentType: 'application/json', body }); });
    await page.goto('/');
    await expect(page.getByRole('alert')).toContainText('Invalid');
    await expect(page.locator('.device-dot')).toHaveClass(/offline/);
    await scenarioScreenshot(page, name.replaceAll(' ', '-'));
    await page.getByRole('button', { name: catalog.en['common.refresh'], exact: true }).click();
    await expect(page.locator('.device-dot')).toHaveClass(/online/);
    await expect(page.getByRole('alert')).toHaveCount(0);
  });
}

test('connection reset is visible and recovery does not reload the page', async ({ page, api }) => {
  api.expectedHttpErrors.add('/api/status');
  api.once('GET /api/status', async route => { await route.abort('connectionreset'); });
  await page.goto('/');
  await expect(page.getByRole('alert')).toContainText('request failed');
  await page.getByRole('button', { name: catalog.en['common.refresh'], exact: true }).click();
  await expect(page.locator('.device-dot')).toHaveClass(/online/);
  await expect(page.getByRole('alert')).toHaveCount(0);
});

test('the actual extended-request timeout releases the queue and permits recovery', async ({ page, api }) => {
  const pending = gate();
  api.expectedHttpErrors.add('/api/status');
  api.once('GET /api/status', async route => { await pending.promise; await route.abort('aborted'); });
  try {
    await page.goto('/');
    await expect.poll(() => api.requests.filter(r => r.startsWith('GET /api/status')).length).toBe(1);
    await page.clock.fastForward(12001);
    await expect(page.getByRole('alert')).toContainText('timed out');
    pending.release();
    await page.getByRole('button', { name: catalog.en['common.refresh'], exact: true }).click();
    await expect(page.locator('.device-dot')).toHaveClass(/online/);
    await expect(page.getByRole('alert')).toHaveCount(0);
  } finally { pending.release(); }
});

for (const environment of ['hidden', 'offline'] as const) {
  test(`${environment} pauses polling; resume performs one immediate read`, async ({ page, api, context }) => {
    api.status.workflow!.step = 'OBJECT';
    await page.goto('/');
    await expect(page.locator('.device-dot')).toHaveClass(/online/);
    const count = () => api.requests.filter(r => r.startsWith('GET /api/status')).length;
    if (environment === 'offline') {
      await context.setOffline(true);
      await expect.poll(() => page.evaluate(() => navigator.onLine)).toBe(false);
    } else await page.evaluate(() => {
      Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' });
      document.dispatchEvent(new Event('visibilitychange'));
    });
    const before = count();
    await page.clock.fastForward(60000);
    expect(count()).toBe(before);
    if (environment === 'offline') await context.setOffline(false);
    else await page.evaluate(() => {
      Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'visible' });
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await expect.poll(count).toBe(before + 1);
    await expect(page.locator('.device-dot')).toHaveClass(/online/);
    await scenarioScreenshot(page, `${environment}-recovered`);
  });
}

test('a disappearing sensor removes calibration admission and its return restores it', async ({ page, api }) => {
  const pods = structuredClone(api.status.pods);
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  await page.goto('/');
  const calibrate = page.locator('[data-command="GO_CALIBRATION"]');
  await expect(calibrate).toBeVisible();
  api.status.pods = [];
  api.status.system.ready_pods = 0;
  Object.assign(api.status.workflow!, { sensor_sufficiency: 'INSUFFICIENT', allowed_actions: ['BACK'] });
  await page.clock.fastForward(5001);
  await expect(calibrate).toHaveCount(0);
  await scenarioScreenshot(page, 'sensor-disappeared');
  api.status.pods = pods;
  api.status.system.ready_pods = pods.length;
  Object.assign(api.status.workflow!, { sensor_sufficiency: 'CAPABLE_3D', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  await page.clock.fastForward(5001);
  await expect(calibrate).toBeVisible();
});
