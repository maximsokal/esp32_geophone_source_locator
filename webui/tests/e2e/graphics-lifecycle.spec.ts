import { test, expect } from './fixtures/device';
import { scenarioScreenshot } from './fixtures/scenarios';
import { installGraphicsProbe, journal, rendered, released, assertReleased } from './helpers/graphics';
import type { Page } from '@playwright/test';

async function open(page: Page, syntheticXR = false) {
  await page.goto(`/stress/scan-result-ui.html${syntheticXR ? '?syntheticXR=1' : ''}`);
  await expect(page.locator('#fixture-state')).toHaveAttribute('data-loaded', 'true');
  await expect(page.locator('#fixture-state')).toHaveText('289 recorded points');
  await rendered(page);
}

test.beforeEach(async ({ page, api }) => {
  api.overrides.set('GET /stress/scan-result-ui.html', route => route.continue());
  api.overrides.set('GET /stress/xr-mobile-stress.html', route => route.continue());
  api.overrides.set('GET /stress/xr-structural-stress.html', route => route.continue());
  await page.addInitScript(installGraphicsProbe);
});

test.afterEach(async ({ page, browser }, info) => {
  const value = await journal(page);
  await info.attach('graphics-journal', { contentType: 'application/json', body: JSON.stringify({
    browser: browser.version(), viewport: page.viewportSize(), ...value,
  }, null, 2) });
  expect(value.workers.filter(worker => worker.errors > 0), 'No Worker failure hidden by an optional fallback').toEqual([]);
});

for (const locale of ['en', 'uk'] as const) test.describe(locale, () => {
  test.use({ appLocale: locale });
  test('recorded scan renders all layers, emitted mesh Workers, camera presets and resize', async ({ page }) => {
    await open(page);
    for (const mode of ['quality', 'surface', 'volume', 'mesh']) {
      await page.locator(`#mode-${mode}`).click();
      await rendered(page);
      if (mode === 'mesh') {
        await expect.poll(async () => (await journal(page)).workers.filter(worker => worker.requests.includes('reconstruct') && worker.triangles.some(count => count > 0)).length,
          { timeout: 20_000, message: 'Actual emitted TSDF Worker finishes; no synchronous substitute' }).toBeGreaterThan(0);
        await expect(page.locator('.scan-mesh-overlay')).not.toHaveAttribute('data-mesh-quality', 'pending');
        const context = (await journal(page)).contexts.filter(value => value.connected).at(-1)!;
        await expect.poll(async () => (await journal(page)).contexts[context.id].draws).toBeGreaterThan(context.draws);
      }
      await scenarioScreenshot(page, `${locale}-${mode}`);
    }
    for (const preset of ['top', 'front', '3d']) {
      await page.locator(`#view-${preset}`).click();
      await scenarioScreenshot(page, `${locale}-${preset}`);
    }
    for (const viewport of [{ width: 852, height: 393 }, { width: 360, height: 800 }]) {
      await page.setViewportSize(viewport);
      await expect.poll(() => page.locator('.workspace canvas').first().evaluate(canvas => {
        const rect = canvas.getBoundingClientRect(); return rect.width > 0 && rect.width <= innerWidth && rect.height > 0;
      })).toBe(true);
      await scenarioScreenshot(page, `${locale}-${viewport.width}x${viewport.height}`);
    }
    const value = await journal(page);
    expect(value.workers.some(worker => worker.requests.includes('decode') && worker.responses.includes('result') && !worker.active)).toBe(true);
    expect(value.contexts.some(context => (context.created.Texture ?? 0) > 0 && (context.created.Buffer ?? 0) > 0 && (context.created.Program ?? 0) > 0)).toBe(true);
    await page.locator('#mount').click();
    await released(page);
  });
});

for (const renderer of ['scan', 'localization', 'sensors']) {
  test(`${renderer}: controls remain attached after extent updates and all resources close`, async ({ page }) => {
    await open(page);
    await page.locator('#renderer').selectOption(renderer);
    await rendered(page);
    const current = (await journal(page)).contexts.filter(context => context.connected).at(-1)!;
    expect(current.listeners).toBeGreaterThan(0);
    await page.locator('#extent').click();
    await expect.poll(async () => (await journal(page)).contexts[current.id].listeners,
      { message: 'Extent updates must retain the same live OrbitControls listeners' }).toBe(current.listeners);
    const canvas = page.locator('.workspace canvas').first();
    await canvas.scrollIntoViewIfNeeded();
    const before = await canvas.screenshot();
    const box = (await canvas.boundingBox())!;
    await page.mouse.move(box.x + box.width * 0.45, box.y + box.height * 0.45);
    await page.mouse.down(); await page.mouse.move(box.x + box.width * 0.75, box.y + box.height * 0.65, { steps: 10 }); await page.mouse.up();
    await expect.poll(async () => (await canvas.screenshot()).equals(before), { message: 'Real camera drag changes the rendered image' }).toBe(false);
    await scenarioScreenshot(page, `${renderer}-extent-drag`);
    await page.locator('#mount').click();
    await released(page);
  });
}

test('native context loss/restoration resumes pixels and draw calls for every renderer', async ({ page }) => {
  await open(page);
  for (const renderer of ['scan', 'sensors', 'localization']) {
    await page.locator('#renderer').selectOption(renderer);
    await rendered(page);
    const before = (await journal(page)).contexts.filter(context => context.connected).at(-1)!;
    await page.evaluate(id => window.__graphics.lose(id, false), before.id);
    await expect.poll(async () => (await journal(page)).contexts[before.id].losses).toBe(before.losses + 1);
    await page.evaluate(id => window.__graphics.lose(id, true), before.id);
    await expect.poll(async () => (await journal(page)).contexts[before.id].restores).toBe(1);
    await expect.poll(async () => (await journal(page)).contexts[before.id].draws).toBeGreaterThan(before.draws);
    await rendered(page);
    await scenarioScreenshot(page, `${renderer}-native-context-restored`);
  }
  await page.locator('#mount').click();
  await released(page);
});

test('warmup, reference and changed workload repeats record performance without invented thresholds', async ({ page }) => {
  await open(page);
  await page.locator('#mount').click(); await released(page);
  const samples = [];
  for (const phase of ['warmup', 'reference', 'changed'] as const) {
    for (let repeat = 0; repeat < (phase === 'warmup' ? 2 : 3); repeat++) {
      if (phase === 'changed') await page.locator('#extent').click();
      const start = Date.now();
      await page.locator('#mount').click(); await rendered(page);
      const frames = await page.evaluate(() => new Promise<number[]>(resolve => {
        const values: number[] = []; let previous = 0;
        const next = (time: number) => { if (previous) values.push(time - previous); previous = time;
          if (values.length === 20) resolve(values); else requestAnimationFrame(next); };
        requestAnimationFrame(next);
      }));
      await page.locator('#table').click();
      await expect(page.getByRole('caption')).toHaveText('Recorded scan summary (2D)');
      await released(page);
      await page.locator('#table').click(); await rendered(page);
      if (repeat === 0) await scenarioScreenshot(page, `${phase}-reopened-3d`);
      await page.locator('#mount').click(); await released(page);
      const cdp = await page.context().newCDPSession(page);
      await cdp.send('HeapProfiler.collectGarbage');
      const heap = await cdp.send('Runtime.getHeapUsage'); await cdp.detach();
      samples.push({ phase, repeat, mountTransitionCloseMs: Date.now() - start, frameIntervalsMs: frames, heap });
    }
  }
  await test.info().attach('performance-observations', { contentType: 'application/json', body: JSON.stringify({
    environment: (await journal(page)).environment, samples,
    policy: 'Observed browser rAF/heap, not GPU timer/FPS or a performance gate. Compare only within this browser/GPU/DPR partition; no new thresholds.' }, null, 2) });
});

for (const ending of ['operator', 'external', 'context-loss'] as const) {
  test(`synthetic XR ${ending} end returns the actual panel to usable UI and releases native GL`, async ({ page }) => {
    await open(page, true);
    await page.locator('#renderer').selectOption('runtime');
    await expect(page.locator('.xr-start-button')).toBeEnabled();
    await page.locator('.xr-start-button').click();
    await expect(page.locator('#fixture-state')).toHaveAttribute('data-active', 'true');
    await expect(page.locator('.xr-finish-button')).toBeVisible();
    const activeContext = (await journal(page)).contexts.filter(context => !context.lost && context.live > 0).at(-1)!;
    expect(activeContext.created.Program).toBeGreaterThan(0);
    expect(activeContext.created.Buffer).toBeGreaterThan(0);
    await scenarioScreenshot(page, `${ending}-session-running`);
    if (ending === 'operator') await page.locator('.xr-finish-button').click();
    else if (ending === 'external') await page.evaluate(() => (window as unknown as { __syntheticXR: { end: () => void } }).__syntheticXR.end());
    else await page.evaluate(id => window.__graphics.lose(id, false), activeContext.id);
    await expect(page.locator('#fixture-state')).toHaveAttribute('data-active', 'false');
    await expect(page.locator('.xr-finish-button')).toBeHidden();
    await expect(page.locator('.xr-start-button')).toBeEnabled();
    expect(await page.evaluate(() => (window as unknown as { __syntheticXR: { state: () => unknown } }).__syntheticXR.state())).toEqual({ liveSessions: 0, pendingFrames: 0, listeners: 0 });
    await scenarioScreenshot(page, `${ending}-session-ended`);
    await page.locator('#mount').click(); await released(page);
  });
}

test('resource detector rejects an intentionally retained native Buffer and Worker', async ({ page }) => {
  await open(page);
  await page.locator('#mount').click(); await released(page);
  const handle = await page.evaluateHandle(() => {
    const gl = document.createElement('canvas').getContext('webgl2')!;
    const buffer = gl.createBuffer();
    const url = URL.createObjectURL(new Blob(['onmessage = () => postMessage("alive")'], { type: 'application/javascript' }));
    const worker = new Worker(url); URL.revokeObjectURL(url); worker.postMessage('probe');
    const listener = () => {};
    document.addEventListener('pointerup', listener);
    return { gl, buffer, worker, listener };
  });
  try {
    const value = await journal(page);
    expect(value.contexts.at(-1)!.live).toBeGreaterThan(0);
    expect(value.documentControlListeners).toBe(1);
    expect(() => assertReleased({ ...value, workers: [], documentControlListeners: 0 })).toThrow();
    expect(() => assertReleased({ ...value, contexts: [], workers: [] })).toThrow();
    const workerOnly = { ...value, contexts: [], documentControlListeners: 0 };
    expect(() => assertReleased(workerOnly)).toThrow();
  } finally {
    await handle.evaluate(({ gl, buffer, worker, listener }) => {
      document.removeEventListener('pointerup', listener); gl.deleteBuffer(buffer); worker.terminate(); gl.getExtension('WEBGL_lose_context')!.loseContext();
    });
    await handle.dispose();
  }
  await released(page);
});

test('existing mobile and structural stress pages execute their production CPU workloads', async ({ page }) => {
  for (const kind of ['mobile', 'structural']) {
    await page.goto(`/stress/xr-${kind}-stress.html`);
    await expect.poll(() => page.evaluate(() => (window as unknown as { __XR_STRESS_DONE__?: boolean }).__XR_STRESS_DONE__), { timeout: 60_000 }).toBe(true);
    const result = await page.evaluate(() => (window as unknown as { __XR_STRESS_RESULT__: { pass: boolean } }).__XR_STRESS_RESULT__);
    await test.info().attach(`${kind}-existing-stress`, { contentType: 'application/json', body: JSON.stringify(result, null, 2) });
    expect(result.pass).toBe(true);
    await scenarioScreenshot(page, `${kind}-existing-stress`);
  }
});
