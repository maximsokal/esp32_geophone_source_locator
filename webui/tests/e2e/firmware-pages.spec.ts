import { test, expect, openPhonePage, assertFirmwareLayout, firmwareLayoutIssues, type PhoneInputs } from './fixtures/firmware-pages';
import { contract } from './helpers/layout';
import { scenarioScreenshot, gate } from './fixtures/scenarios';

const pages = [['/setup', 'Wi-Fi Setup'], ['/saved', 'Saved'], ['/setup/ca-guide', 'HTTPS certificate'],
  ['/setup/verify-ui?t=1234', 'secure browser verification'], ['/', 'GeoLocator'], ['/seismic', 'Seismic Monitor'],
  ['/setup/ca', 'HTTPS certificate'], ['/setup/ca?installed=1', 'CA installation confirmed'],
  ['/setup/verify?t=1234', 'secure browser verification']] as const;

for (const [width, height] of contract.viewports) {
  test(`firmware menus ${width}x${height}`, async ({ page }) => {
    await page.setViewportSize({ width, height });
    for (const [path, title] of pages) {
      await test.step(path, async () => {
        await openPhonePage(page, path);
        await expect(page.locator('body')).toContainText(title);
        if (path === '/') await expect(page.locator('#api-state')).toHaveText('LIVE');
        if (path === '/seismic') await expect(page.locator('#status')).toHaveText('LIVE');
        await scenarioScreenshot(page, `${width}x${height}-${path.replace(/[^a-z0-9]/gi, '-') || 'fallback'}`);
        await assertFirmwareLayout(page);
      });
    }
  });
}

test('200% text on every firmware page', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  for (const [path] of pages) {
    await openPhonePage(page, path);
    await page.evaluate(() => {
      const sizes = [...document.querySelectorAll<HTMLElement>('body,body *')]
        .map(el => [el, parseFloat(getComputedStyle(el).fontSize) * 2] as const);
      sizes.forEach(([el, size]) => { el.style.fontSize = `${size}px`; });
    });
    await scenarioScreenshot(page, `200percent-${path.replace(/[^a-z0-9]/gi, '-') || 'fallback'}`);
    await assertFirmwareLayout(page);
  }
});

test('Wi-Fi form requires SSID, masks password and posts once before confirmation', async ({ page, firmware }) => {
  await openPhonePage(page, '/setup');
  const ssid = page.getByLabel('Hotspot name');
  const password = page.getByLabel('Hotspot password');
  await expect(password).toHaveAttribute('type', 'password');
  await expect(ssid).toHaveAttribute('maxlength', '32');
  await expect(password).toHaveAttribute('maxlength', '63');
  await page.getByRole('button', { name: 'Save hotspot' }).click();
  expect(firmware.requests.filter(r => r.method === 'POST')).toHaveLength(0);
  expect(await ssid.evaluate(el => (el as HTMLInputElement).validity.valueMissing)).toBe(true);
  await ssid.fill('Field Phone'); await password.fill('field+password');
  await page.getByRole('button', { name: 'Save hotspot' }).click();
  await expect(page.getByRole('heading', { name: 'Saved', exact: true })).toBeVisible();
  const posts = firmware.requests.filter(r => r.method === 'POST');
  expect(posts).toHaveLength(1);
  const body = new URLSearchParams(posts[0].body!);
  expect(Object.fromEntries(body)).toEqual({ ssid: 'Field Phone', password: 'field+password' });
  await assertFirmwareLayout(page);
});

for (const [response, message] of [['wifi-invalid-form', 'Invalid Wi-Fi settings'], ['wifi-save-error', 'Failed to save Wi-Fi settings'],
  ['wifi-ca-required', 'Confirm GeoLocator Root CA'], ['wifi-not-setup', 'only available in setup mode'],
  ['wifi-shutdown', 'Shutdown is in progress']] as const) {
  test(`Wi-Fi production error ${response}`, async ({ page, firmware }) => {
    firmware.wifiResponse = response;
    await openPhonePage(page, '/setup');
    await page.getByLabel('Hotspot name').fill('Field Phone');
    await page.getByLabel('Hotspot password').fill('field+password');
    await page.getByRole('button', { name: 'Save hotspot' }).click();
    await expect(page.locator('body')).toContainText(message);
    expect(firmware.requests.filter(r => r.method === 'POST')).toHaveLength(1);
    await assertFirmwareLayout(page);
  });
}

test('setup redirects to the actual CA guide before credentials are admitted', async ({ page, firmware }) => {
  await openPhonePage(page, '/setup/ca-guide');
  firmware.setupResponse = 'wifi-needs-ca';
  const redirect = firmware.producer.cases.find(reply => reply.id === 'wifi-needs-ca')!;
  expect(redirect.status).toBe(302);
  expect(redirect.location).toBe('http://192.168.4.1/setup/ca-guide');
  expect(await page.evaluate(() => fetch('/setup', { redirect: 'manual' }).then(reply => reply.type))).toBe('opaqueredirect');
  expect(firmware.requests.filter(reply => reply.path === '/setup')).toHaveLength(1);
  await expect(page.getByRole('link', { name: '1. Download certificate file' })).toHaveAttribute('href', '/setup/ca-download');
  await expect(page.getByRole('link', { name: /I installed the CA/ })).toHaveAttribute('href', '/setup/ca?installed=1');
  expect(firmware.requests.filter(r => r.method === 'POST')).toHaveLength(0);
  await assertFirmwareLayout(page);
});
test('CA unavailable has the production failure message', async ({ page }) => {
  await openPhonePage(page, '/setup/ca-guide');
  await page.getByRole('link', { name: '1. Download certificate file' }).click();
  await expect(page.locator('body')).toContainText('GeoLocator Root CA is not embedded');
  await assertFirmwareLayout(page);
});

test('legacy CA page confirms installation with the actual confirmation response', async ({ page }) => {
  await openPhonePage(page, '/setup/ca');
  await expect(page.getByRole('link', { name: 'Download GeoLocator Root CA', exact: true })).toHaveAttribute('href', '/setup/ca.crt');
  await page.getByRole('link', { name: 'I installed the CA - continue', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'CA installation confirmed', exact: true })).toBeVisible();
  await assertFirmwareLayout(page);
});
test('legacy verifier checks token, sends actual reports and permits an explicit repeat', async ({ page, firmware }) => {
  await openPhonePage(page, '/setup/verify');
  await expect(page.locator('body')).toContainText('Invalid or expired GeoLocator verification token');
  await scenarioScreenshot(page, 'legacy-verification-expired');
  await openPhonePage(page, '/setup/verify?t=1234');
  await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
  await expect(page.locator('#result')).toContainText('"ready": true');
  expect(firmware.requests.filter(row => row.method === 'POST')[0].path).toBe('/api/setup/verify?t=1234');
  expect(await page.evaluate(() => window.firmwarePhone.trackStops)).toBe(1);
  firmware.verifyMode = 'incomplete';
  await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
  await expect(page.locator('#result')).toContainText('"ready": false');
  expect(firmware.requests.filter(row => row.method === 'POST')).toHaveLength(2);
  await assertFirmwareLayout(page);
});

const deniedCases: [string, PhoneInputs, string][] = [
  ['insecure', { secure: false }, 'secure_context'], ['camera denied', { camera: 'denied' }, 'camera'],
  ['camera API missing', { camera: 'missing' }, 'camera_api'], ['XR API missing', { xr: 'missing' }, 'xr_api'],
  ['AR unsupported', { xr: 'unsupported' }, 'immersive_ar'], ['XR check throws', { xr: 'throws' }, 'immersive_ar'],
];
for (const [name, inputs, failed] of deniedCases) {
  test(`phone verification ${name}`, async ({ page, firmware }) => {
    await openPhonePage(page, '/setup/verify-ui?t=1234&boot=7', inputs);
    await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Verification incomplete', exact: true })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Verify again', exact: true })).toBeEnabled();
    await expect(page.getByRole('link', { name: 'Open GeoLocator dashboard' })).toHaveCount(0);
    const requests = firmware.requests.filter(r => r.method === 'POST');
    expect(requests).toHaveLength(1);
    expect(requests[0].path).toBe('/api/setup/verify?t=1234&boot=7');
    expect(JSON.parse(requests[0].body!)[failed]).toBe(false);
    await page.getByText('Technical details', { exact: true }).click();
    await assertFirmwareLayout(page);
  });
}
for (const mode of ['http', 'malformed', 'offline'] as const) {
  test(`phone verification ${mode} response retries explicitly`, async ({ page, firmware }) => {
    firmware.verifyMode = mode;
    await openPhonePage(page, '/setup/verify-ui?t=1234');
    await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Verification incomplete', exact: true })).toBeVisible();
    await scenarioScreenshot(page, `verification-${mode}`);
    firmware.verifyMode = 'ready';
    await page.getByRole('button', { name: 'Verify again', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Verification completed successfully' })).toBeVisible();
    expect(firmware.requests.filter(r => r.method === 'POST')).toHaveLength(2);
    await assertFirmwareLayout(page);
  });
}

test('expired token and server refusal keep verification incomplete when phone checks pass', async ({ page, firmware }) => {
  await openPhonePage(page, '/setup/verify-ui?t=expired');
  await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Verification incomplete', exact: true })).toBeVisible();
  expect(firmware.requests.find(row => row.method === 'POST')!.path).toBe('/api/setup/verify?t=expired');
  await scenarioScreenshot(page, 'verification-expired-token');
  firmware.verifyMode = 'incomplete';
  await openPhonePage(page, '/setup/verify-ui?t=1234');
  await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Verification incomplete', exact: true })).toBeVisible();
  expect(JSON.parse(firmware.requests.find(r => r.method === 'POST')!.body!)).toMatchObject({
    secure_context: true, camera: true, camera_api: true, xr_api: true, immersive_ar: true,
  });
  await expect(page.getByRole('link', { name: 'Open GeoLocator dashboard' })).toHaveCount(0);
  await assertFirmwareLayout(page);
});

test('successful phone verification stops camera tracks and prevents repeated submission', async ({ page, firmware }) => {
  const pending = gate(); firmware.verifyHold = pending.promise;
  await openPhonePage(page, '/setup/verify-ui?t=1234&boot=7');
  await page.getByRole('button', { name: 'Verify phone', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Checking...', exact: true })).toBeDisabled();
  await expect.poll(() => firmware.requests.filter(r => r.method === 'POST').length).toBe(1);
  await page.keyboard.press('Enter');
  expect(firmware.requests.filter(r => r.method === 'POST')).toHaveLength(1);
  await scenarioScreenshot(page, 'verification-pending'); pending.release();
  await expect(page.getByRole('heading', { name: 'Verification completed successfully' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Verified', exact: true })).toBeDisabled();
  expect(await page.evaluate(() => window.firmwarePhone)).toEqual({ cameraCalls: 1, trackStops: 1, xrCalls: ['immersive-ar'] });
  await page.getByText('Technical details', { exact: true }).click();
  await assertFirmwareLayout(page);
  await scenarioScreenshot(page, 'verification-success');
  await page.getByRole('link', { name: 'Open GeoLocator dashboard' }).click();
  await expect(page.locator('#api-state')).toHaveText('LIVE');
});

for (const mode of ['http', 'malformed'] as const) {
  for (const section of ['fallback', 'seismic'] as const) {
    test(`${section} ${mode} failure recovers through real polling`, async ({ page, firmware }) => {
      if (section === 'fallback') firmware.statusMode = mode; else firmware.seismicMode = mode;
      await openPhonePage(page, section === 'fallback' ? '/' : '/seismic');
      const status = page.locator(section === 'fallback' ? '#api-state' : '#status');
      await expect(status).toHaveText('OFFLINE');
      await scenarioScreenshot(page, `${section}-${mode}-offline`);
      firmware.statusMode = firmware.seismicMode = 'ok';
      await expect(status).toHaveText('LIVE');
      await assertFirmwareLayout(page);
      if (section === 'seismic') {
        await page.getByRole('link', { name: 'Dashboard' }).click();
        await expect(page.locator('#api-state')).toHaveText('LIVE');
      }
    });
  }
}
for (const [source, expected] of [['AD7606', 'LIVE'], ['TEST', 'TEST SIGNAL'], ['NONE', 'WAITING']] as const) {
  test(`seismic ${source} renders four real canvases`, async ({ page, firmware }) => {
    firmware.seismicSource = source;
    await openPhonePage(page, '/seismic');
    await expect(page.locator('#status')).toHaveText(expected);
    await expect(page.locator('canvas')).toHaveCount(4);
    for (const canvas of await page.locator('canvas').all()) {
      expect(await canvas.evaluate(el => { const c = el as HTMLCanvasElement;
        return [...c.getContext('2d')!.getImageData(0, 0, c.width, c.height).data].some(value => value !== 0); })).toBe(true);
    }
    await assertFirmwareLayout(page);
  });
}
test('firmware geometry detector rejects undersized controls and overflow', async ({ page }) => {
  await openPhonePage(page, '/setup');
  await assertFirmwareLayout(page);
  const tiny = await page.addStyleTag({ content: 'input { min-height:0!important; height:10px!important; padding:0!important }' });
  const tinyIssues = await firmwareLayoutIssues(page);
  expect(tinyIssues.some(issue => issue.startsWith('target '))).toBe(true);
  const wide = await page.addStyleTag({ content: 'form { width:1500px!important }' });
  const wideIssues = await firmwareLayoutIssues(page);
  expect(wideIssues.some(issue => issue.startsWith('page overflow:'))).toBe(true);
  await test.info().attach('negative-controls', { contentType: 'application/json', body: JSON.stringify({ tinyIssues, wideIssues }) });
  await wide.evaluate(el => el.parentNode?.removeChild(el)); await tiny.evaluate(el => el.parentNode?.removeChild(el));
  await assertFirmwareLayout(page);
});
