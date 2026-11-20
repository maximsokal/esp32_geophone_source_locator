import { chromium, expect, test } from '@playwright/test';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { rasterTag } from './support/raster';

const scratch = resolve(import.meta.dirname, '../../../.test-build/tag-lab-e2e');
const logsRoot = resolve(import.meta.dirname, '../../../logs/tag-lab');
const W = 1280; const H = 720;

/** Deterministic camera recording: tag id 2 on white, mid-frame, as 4:2:0 y4m (Chromium loops it as the fake webcam). */
function writeTagY4m(tagId: number): string {
  mkdirSync(scratch, { recursive: true });
  const raster = rasterTag(tagId, { width: W, height: H, left: 400, top: 200, cell: 20 });
  const y = new Uint8Array(W * H);
  for (let i = 0; i < W * H; i += 1) y[i] = raster.rgba[i * 4];
  const chroma = new Uint8Array((W / 2) * (H / 2)).fill(128);
  const header = Buffer.from(`YUV4MPEG2 W${W} H${H} F10:1 Ip A1:1 C420jpeg\n`);
  const frame = Buffer.concat([Buffer.from('FRAME\n'), Buffer.from(y), Buffer.from(chroma), Buffer.from(chroma)]);
  const path = resolve(scratch, `tag${tagId}.y4m`);
  writeFileSync(path, Buffer.concat([header, ...Array.from({ length: 10 }, () => frame)]));
  return path;
}

async function runId(request: import('@playwright/test').APIRequestContext): Promise<string> {
  return (await (await request.get('/lab-log/health')).json()).runId as string;
}

function events(id: string): { event: string; fields: Record<string, unknown> }[] {
  const path = resolve(logsRoot, id, 'events.jsonl');
  return existsSync(path) ? readFileSync(path, 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l)) : [];
}

test('synthetic replay: real worker decodes tags and the overlay is drawn over the live image', async ({ page, request }) => {
  await page.goto('/tag-lab.html?source=synthetic&autostart=1');
  await expect(page.getByTestId('tag-lab-stop')).toBeEnabled();
  await expect.poll(async () => Number(await page.getByTestId('tag-lab-decoded').innerText()), { timeout: 20_000 }).toBeGreaterThan(2);
  const detected = page.locator('[data-status="DETECTED 2D"]');
  await expect(detected.first()).toBeVisible();
  const painted = await page.getByTestId('tag-lab-overlay').evaluate((canvas: HTMLCanvasElement) => {
    const ctx = canvas.getContext('2d')!;
    const data = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
    let count = 0; for (let i = 3; i < data.length; i += 4) if (data[i] > 0) count += 1;
    return count;
  });
  expect(painted).toBeGreaterThan(200);
  await page.getByTestId('tag-lab-stop').click();
  await expect(page.getByTestId('tag-lab-start')).toBeEnabled();
  await expect(page.getByTestId('tag-lab-errors')).toContainText('none');
  const id = await runId(request);
  await expect.poll(() => events(id).some((e) => e.event === 'TAG_DETECTED'), { timeout: 10_000 }).toBe(true);
  await expect.poll(() => events(id).some((e) => e.event === 'SESSION_STOP')).toBe(true);
  expect(existsSync(resolve(logsRoot, id, 'run-metadata.json'))).toBe(true);
});

test('webcam path: getUserMedia -> frames -> real worker -> labelled overlay, tracks released on stop', async ({ request }) => {
  const y4m = writeTagY4m(2);
  const browser = await chromium.launch({ args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', `--use-file-for-fake-video-capture=${y4m}`] });
  try {
    const context = await browser.newContext({ baseURL: 'http://127.0.0.1:5188', viewport: { width: 1500, height: 900 } });
    const page = await context.newPage();
    await page.goto('/tag-lab.html');
    await page.getByTestId('tag-lab-start').click();
    await expect(page.getByTestId('tag-lab-status-2')).toHaveAttribute('data-status', 'DETECTED 2D');
    await expect(page.getByTestId('tag-lab-last')).toContainText('G2 (id 2)');
    await expect(page.getByTestId('tag-lab-errors')).toContainText('none');
    const live = await page.getByTestId('tag-lab-video').evaluate((v: HTMLVideoElement) => (v.srcObject as MediaStream).getVideoTracks().map((t) => t.readyState));
    expect(live).toEqual(['live']);
    await page.getByTestId('tag-lab-stop').click();
    const afterStop = await page.getByTestId('tag-lab-video').evaluate((v: HTMLVideoElement) => v.srcObject === null);
    expect(afterStop).toBe(true);
    const id = await runId(request);
    await expect.poll(() => events(id).some((e) => e.event === 'CAMERA_READY')).toBe(true);
    const detected = events(id).filter((e) => e.event === 'TAG_DETECTED' && (e.fields as { tagId?: number }).tagId === 2);
    expect(detected.length).toBeGreaterThan(0);
    await context.close();
  } finally { await browser.close(); }
});

test('missing camera permission is shown in the error panel and logged', async ({ browser, request }) => {
  const context = await browser.newContext({ baseURL: 'http://127.0.0.1:5188', permissions: [] });
  const page = await context.newPage();
  await page.addInitScript(() => {
    navigator.mediaDevices.getUserMedia = () => Promise.reject(Object.assign(new Error('denied for test'), { name: 'NotAllowedError' }));
  });
  await page.goto('/tag-lab.html');
  await page.getByTestId('tag-lab-start').click();
  await expect(page.getByTestId('tag-lab-errors')).toContainText('permission denied');
  await expect(page.getByTestId('tag-lab-start')).toBeEnabled();
  const id = await runId(request);
  await expect.poll(() => events(id).some((e) => e.event === 'LAB_ERROR')).toBe(true);
  await context.close();
});
