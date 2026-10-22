import { test, expect, catalog } from './fixtures/device';
import { gate, scenarioScreenshot } from './fixtures/scenarios';

test('leaving during a delayed real scanner chunk does not reopen it after completion', async ({ page, api, request }) => {
  const manifest = await (await request.get('/.vite/manifest.json')).json();
  const asset = '/' + manifest['src/App.tsx'].file;
  const pending = gate();
  api.once(`GET ${asset}`, async route => { await pending.promise; await route.continue(); });
  try {
    await page.goto('/');
    await page.locator('[data-command="SCAN_START"]').click();
    await expect(page.getByText(catalog.en['console.loading_xr'], { exact: true })).toBeVisible();
    await scenarioScreenshot(page, 'scanner-chunk-loading');
    await page.getByRole('button', { name: catalog.en['console.back_object'], exact: true }).click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.scan_choice'], exact: true })).toBeVisible();
    const loaded = page.waitForResponse(r => new URL(r.url()).pathname === asset);
    pending.release();
    await loaded;
    await page.evaluate(() => new Promise<void>(resolve => requestAnimationFrame(() => resolve())));
    await expect(page.locator('.scanner-feature-shell')).toHaveCount(0);
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.scan_choice'], exact: true })).toBeVisible();
    expect(api.controls.map(c => c.command)).toEqual(['SCAN_START', 'BACK']);
  } finally { pending.release(); }
});

test('chunk failure has bounded retry/reload and the session reload guard survives navigation', async ({ page, api, request, baseURL }) => {
  const manifest = await (await request.get('/.vite/manifest.json')).json();
  const asset = '/' + manifest['src/App.tsx'].file;
  api.expectedHttpErrors.add(asset);
  api.expectedConsoleErrors.push(`TypeError: Failed to fetch dynamically imported module: ${baseURL}${asset}`);
  api.overrides.set(`GET ${asset}`, async route => { await route.fulfill({ status: 503, body: 'injected scanner chunk unavailable' }); });
  await page.goto('/');
  await page.locator('[data-command="SCAN_START"]').click();
  await expect(page.getByRole('alert')).toContainText(catalog.en['console.scanner.chunk_load_failed']);
  for (let attempt = 0; attempt < 3; attempt += 1) {
    await page.getByRole('button', { name: catalog.en['seismic.retry'], exact: true }).click();
    await expect(page.getByRole('alert')).toBeVisible();
  }
  await expect(page.getByRole('button', { name: catalog.en['seismic.retry'], exact: true })).toHaveCount(0);
  await scenarioScreenshot(page, 'scanner-chunk-failed');
  await page.getByRole('button', { name: catalog.en['console.scanner.reload_page'], exact: true }).click();
  await expect(page.getByRole('alert')).toContainText(catalog.en['console.scanner.chunk_load_failed']);
  await expect(page.getByRole('button', { name: catalog.en['console.scanner.reload_page'], exact: true })).toHaveCount(0);
  expect(await page.evaluate(() => sessionStorage.getItem('geolocator.scanner.chunkReloadAttempted'))).toBe('1');
  await page.getByRole('button', { name: catalog.en['console.back_object'], exact: true }).first().click();
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.scan_choice'], exact: true })).toBeVisible();
});

test('unavailable immersive capability keeps the real scanner inert and return usable', async ({ page, api }) => {
  await page.addInitScript(() => {
    Object.defineProperty(navigator, 'xr', { configurable: true, value: {
      isSessionSupported: async () => false,
      requestSession: async () => { throw new Error('Unsupported XR must not request a session'); },
    } });
  });
  await page.goto('/');
  await page.locator('[data-command="SCAN_START"]').click();
  await expect(page.getByRole('heading', { name: catalog.en['scan.search_volume_title'], exact: true })).toBeVisible();
  await expect(page.getByText(catalog.en['xr.value.checking'], { exact: false })).toHaveCount(0);
  await expect(page.getByRole('button', { name: catalog.en['xr.start_scan'], exact: true })).toBeDisabled();
  await scenarioScreenshot(page, 'xr-unavailable');
  await page.getByRole('button', { name: catalog.en['console.back_object'], exact: true }).click();
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.scan_choice'], exact: true })).toBeVisible();
  expect(api.controls.map(c => c.command)).toEqual(['SCAN_START', 'BACK']);
});
