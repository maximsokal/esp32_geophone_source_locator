import { test, expect, catalog } from './fixtures/device';
import { enterObjects, gate, savedSnapshot, scenarioScreenshot, seismicResponses } from './fixtures/scenarios';

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    test('saved waveform opens, zooms, moves and closes through the production parser', async ({ page, api }) => {
      seismicResponses(api);
      const t = catalog[locale];
      await enterObjects(page, api, locale);
      await page.locator('.session-row').first().click();
      await expect(page.locator('.saved-seismic-viewer svg')).toHaveCount(4);
      await expect(page.locator('.saved-seismic-viewer path').first()).not.toHaveAttribute('d', '');
      await page.getByRole('button', { name: t['seismic.zoom_in'], exact: true }).click();
      await expect.poll(() => api.requests.some(r => r.includes('start_frame=25&frame_count=50'))).toBe(true);
      await expect(page.getByRole('button', { name: t['seismic.next_range'], exact: true })).toBeEnabled();
      await page.getByRole('button', { name: t['seismic.next_range'], exact: true }).click();
      await expect.poll(() => api.requests.some(r => r.includes('start_frame=50&frame_count=50'))).toBe(true);
      await scenarioScreenshot(page, `${locale}-saved-waveform`);
      await page.getByRole('button', { name: t['seismic.back_to_sessions'], exact: true }).click();
      await expect(page.locator('.saved-seismic-viewer')).toHaveCount(0);
      expect(api.controls).toHaveLength(0);
    });
  });
}

test('saved waveform HTTP and malformed payload failures offer a working retry', async ({ page, api }) => {
  seismicResponses(api);
  api.expectedHttpErrors.add('/api/seismic/session');
  let attempts = 0;
  api.overrides.set('GET /api/seismic/session', async route => {
    attempts += 1;
    if (attempts === 1) await route.fulfill({ status: 503, json: { error: 'storage_busy' } });
    else await route.fulfill({ json: attempts === 2 ? { points: [] } : savedSnapshot() });
  });
  await enterObjects(page, api);
  await page.locator('.session-row').first().click();
  const viewer = page.locator('.saved-seismic-viewer');
  await expect(viewer.getByRole('alert')).toContainText('503');
  await scenarioScreenshot(page, 'saved-http-error');
  await viewer.getByRole('button', { name: catalog.en['seismic.retry'], exact: true }).click();
  await expect(viewer.getByRole('alert')).toContainText('invalid session shape');
  await viewer.getByRole('button', { name: catalog.en['seismic.retry'], exact: true }).click();
  await expect(viewer.getByRole('alert')).toHaveCount(0);
  await expect(viewer.locator('svg')).toHaveCount(4);
  expect(attempts).toBe(3);
});

test('closing a pending saved read releases transport before opening another session', async ({ page, api }) => {
  seismicResponses(api);
  api.status.recording_sessions!.sessions.push({ ...api.status.recording_sessions!.sessions[0], session_id: 'S000002' });
  const pending = gate();
  let cancelled = false;
  page.on('requestfailed', request => { if (request.url().includes('/api/seismic/session') && request.url().includes('session_id=S000001')) cancelled = true; });
  api.once('GET /api/seismic/session', async route => { await pending.promise; await route.abort('aborted'); });
  try {
    await enterObjects(page, api);
    await page.locator('.session-row').filter({ hasText: 'S000001' }).click();
    await expect.poll(() => api.requests.some(r => r.startsWith('GET /api/seismic/session'))).toBe(true);
    await page.getByRole('button', { name: catalog.en['seismic.back_to_sessions'], exact: true }).click();
    await expect.poll(() => cancelled).toBe(true);
    await page.locator('.session-row').filter({ hasText: 'S000002' }).click();
    await expect(page.locator('.saved-seismic-viewer').getByRole('heading', { name: 'S000002', exact: true })).toBeVisible();
    await expect(page.locator('.saved-seismic-viewer svg')).toHaveCount(4);
    pending.release();
    await expect(page.locator('.saved-seismic-viewer').getByRole('heading', { name: 'S000002', exact: true })).toBeVisible();
    await scenarioScreenshot(page, 'new-session-after-cancel');
  } finally { pending.release(); }
});

test.describe('live transport', () => {
  test.use({ manualClock: true });
  test('live failure backs off, recovers and stops polling after close', async ({ page, api }) => {
    api.expectedHttpErrors.add('/api/seismic');
    let reads = 0;
    api.overrides.set('GET /api/seismic', async route => {
      reads += 1;
      if (reads === 1) await route.fulfill({ status: 503, json: { error: 'acquisition_busy' } });
      else await route.fulfill({ json: { source: 'AD7606', sample_rate_hz: 500, count: 2, frame_count: 100,
        requested_span_ms: 440, span_ms: 200, decimation_factor: 50, buffered_frames: 100, total_frames: 100,
        points: savedSnapshot().points } });
    });
    await enterObjects(page, api);
    Object.assign(api.status.workflow!, { step: 'RECORD_READY', allowed_actions: ['START_RECORDING', 'BACK'] });
    await page.clock.fastForward(5001);
    await page.getByRole('button', { name: `${catalog.en['objects.show']} ${catalog.en['objects.live_signal']}`, exact: true }).click();
    await expect(page.locator('.seismic-live-card .console-error')).toContainText('503');
    expect(reads).toBe(1);
    await page.clock.fastForward(1501);
    await expect(page.locator('.seismic-live-card path').first()).not.toHaveAttribute('d', '');
    await expect(page.locator('.seismic-live-card .console-error')).toHaveCount(0);
    await scenarioScreenshot(page, 'live-recovered');
    await page.getByRole('button', { name: `${catalog.en['objects.hide']} ${catalog.en['objects.live_signal']}`, exact: true }).click();
    const before = reads;
    await page.clock.fastForward(60000);
    expect(reads).toBe(before);
    await expect(page.locator('.seismic-live-card')).toHaveCount(0);
  });
});
