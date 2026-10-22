import { test, expect, type Page } from '@playwright/test';
import { catalog } from './device';
import type { DeviceApi } from './api';
import type { Locale } from '../helpers/layout';

export function gate() {
  let release!: () => void;
  const promise = new Promise<void>(resolve => { release = resolve; });
  return { promise, release };
}

export async function scenarioScreenshot(page: Page, name: string) {
  await page.evaluate(() => document.fonts.ready);
  const path = test.info().outputPath(`${name}.png`);
  await page.screenshot({ path, fullPage: true, animations: 'disabled', caret: 'hide' });
  await test.info().attach('visual-actual', { path, contentType: 'image/png' });
}

export async function enterObjects(page: Page, api: DeviceApi, locale: Locale = 'en') {
  Object.assign(api.status.workflow!, { step: 'OBJECT', allowed_actions: ['START_MEASUREMENT', 'BACK'] });
  await page.goto('/');
  await expect(page.locator('.device-dot')).toHaveClass(/online/);
  await page.getByRole('navigation').getByRole('button', { name: catalog[locale]['nav.objects'], exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Quarry', exact: true })).toBeVisible();
}

/** The simulated server owns its model independently of the client's mutation projection. */
export function objectCommands(api: DeviceApi) {
  api.overrides.set('POST /api/control', async route => {
    const body = route.request().postDataJSON() as Record<string, unknown>;
    const store = api.status.object_store!;
    const id = String(body.object_id ?? 'OBJ_0002');
    const object = store.objects.find(o => o.id === id);
    if (body.action === 'create_object') {
      store.objects.push({ ...structuredClone(store.objects[0]), id, name: String(body.name), active: false });
    } else if (body.action === 'rename_object' && object) object.name = String(body.name);
    else if (body.action === 'delete_object') store.objects = store.objects.filter(o => o.id !== id);
    else if (body.action !== 'activate_object') throw new Error(`Unexpected control: ${body.action}`);
    if (body.action === 'create_object' || body.action === 'activate_object') {
      store.active_id = id;
      for (const entry of store.objects) entry.active = entry.id === id;
      api.status.workflow!.object_id = id;
    }
    if (body.action === 'delete_object' && store.active_id === id) store.active_id = null;
    await route.fulfill({ json: { ok: true, action: body.action, object_id: id } });
  });
}

export function savedSnapshot(objectId = 'OBJ_0001', sessionId = 'S000001', start = 0, count = 100) {
  return {
    object_id: objectId, session_id: sessionId, source: 'AD7606', status: 'complete',
    complete: true, readable: true, recovered: false, sample_rate_hz: 500, channel_count: 4,
    samples_written: 100, dropped_frames: 0, recording_ms: 200, source_frame_count: 100,
    start_frame: start, frame_count: count, count: 2, start_timestamp_us: start * 2000,
    end_timestamp_us: (start + count) * 2000,
    points: [0, 1].map(i => ({ timestamp_us: (start + i * Math.floor(count / 2)) * 2000,
      end_timestamp_us: (start + (i + 1) * Math.floor(count / 2)) * 2000,
      channels: [1, 2, 3, 4].map(id => ({ id, min_uv: -id * 10, max_uv: id * 10 })) })),
  };
}

export function seismicResponses(api: DeviceApi) {
  api.overrides.set('POST /api/control', async route => {
    const operation = new URL(route.request().url()).searchParams.get('scan');
    if (operation === 'info') await route.fulfill({ json: { ok: false, error: 'ESP_ERR_NOT_FOUND' } });
    else if (operation === 'load') {
      api.expectedHttpErrors.add('/api/control');
      await route.fulfill({ status: 404, json: { error: 'ESP_ERR_NOT_FOUND' } });
    } else throw new Error('Unexpected seismic metadata mutation');
  });
  api.overrides.set('GET /api/seismic/session', async route => {
    const q = new URL(route.request().url()).searchParams;
    await route.fulfill({ json: savedSnapshot(q.get('object_id')!, q.get('session_id')!,
      Number(q.get('start_frame') ?? 0), Number(q.get('frame_count') ?? 100)) });
  });
  api.overrides.set('GET /api/localization/runs', async route => {
    await route.fulfill({ json: { geometry: { available: false, pods: [] }, cs_pairs: { available: false, pairs: [] }, picks: [], runs: [] } });
  });
}
