import { expect, test, type APIRequestContext } from '@playwright/test';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';

const logsRoot = resolve(import.meta.dirname, '../../../logs/phone-lab');

async function runDir(request: APIRequestContext): Promise<string> {
  const health = await (await request.get('/lab/health')).json() as { runId: string };
  return resolve(logsRoot, health.runId);
}
const lines = (path: string) => (existsSync(path) ? readFileSync(path, 'utf8').trim().split('\n').filter(Boolean) : []);

test('the real app loads on the fake device API and the phone telemetry reaches the log folder', async ({ page, request }) => {
  const dir = await runDir(request);
  await page.goto('/');
  await expect(page.locator('.device-dot')).toHaveClass(/online/);
  await expect.poll(() => lines(resolve(dir, 'events.jsonl')).some((l) => JSON.parse(l).type === 'DEVICE_INFO'), { timeout: 15_000 }).toBe(true);
  const info = lines(resolve(dir, 'events.jsonl')).map((l) => JSON.parse(l)).find((e) => e.type === 'DEVICE_INFO');
  expect(info.data.secure).toBe(true);
  expect(info.data.userAgent).toContain('Android');
  await expect(page.locator('[data-phone-lab="mark"]')).toBeVisible();
  await page.locator('[data-phone-lab="mark"]').click();
  await expect.poll(() => lines(resolve(dir, 'events.jsonl')).some((l) => JSON.parse(l).type === 'LAB_MARK'), { timeout: 10_000 }).toBe(true);
  // The production client-log channel (XR_* diagnostics) lands in the same folder.
  await page.evaluate(() => fetch('/api/client-log', { method: 'POST', body: 'INFO|1|PHONE_LAB_SMOKE\n' }));
  await expect.poll(() => lines(resolve(dir, 'client-log.txt')).some((l) => l.includes('PHONE_LAB_SMOKE'))).toBe(true);
  const requests = lines(resolve(dir, 'requests.jsonl')).map((l) => JSON.parse(l));
  expect(requests.some((r) => r.path === '/api/status' && r.status === 200)).toBe(true);
});

test('scan artifacts saved by the phone are stored, round-trip byte-exact and are copied into the run folder', async ({ request }) => {
  const dir = await runDir(request);
  const q = 'object_id=OBJ_0001&session_id=S000001';
  const payload = Buffer.from(Array.from({ length: 4096 }, (_, i) => (i * 31 + 7) & 0xff));
  const save = await request.post(`/api/control?scan=save&${q}`, { data: payload, headers: { 'content-type': 'application/octet-stream' } });
  expect(save.status()).toBe(200);
  const info = await (await request.post(`/api/control?scan=info&${q}`)).json();
  expect(info).toMatchObject({ ok: true, bytes: 4096 });
  const load = await request.post(`/api/control?scan=load&${q}`);
  expect(Buffer.from(await load.body()).equals(payload)).toBe(true);
  expect(readdirSync(resolve(dir, 'saves')).some((name) => name.endsWith('_scan_4096.bin'))).toBe(true);
  expect((await request.post('/api/control?scan=info&object_id=../x&session_id=S1')).status()).toBe(400);
  await request.post(`/api/control?scan=delete&${q}`);
  expect(await (await request.post(`/api/control?scan=info&${q}`)).json()).toMatchObject({ ok: false, error: 'ESP_ERR_NOT_FOUND' });
});
