// Before/After screenshots of the REAL scan viewer on one scan artifact, identical camera, size, device scale and (software) lighting at every commit.
//
//   node scripts/capture-scan-viewer.mjs --label <name> [--scan <path to *_scan_*.bin>] [--out <dir>]
//
// It starts the Vite dev server of the CURRENT checkout, opens stress/scan-compare.html in Chromium (SwiftShader, 393x852, scale 1) and writes one PNG per
// view plus a JSON file with the viewer's own mesh status. The images are SwiftShader renders, not a phone.
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { chromium } from '@playwright/test';
import { createServer } from 'vite';

const here = dirname(fileURLToPath(import.meta.url));
const webui = resolve(here, '..');
const repo = resolve(webui, '..');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i].replace(/^--/, ''), process.argv[i + 1]);
const label = args.get('label') ?? 'working-tree';
const out = resolve(args.get('out') ?? join(repo, '.test-build/phase2/screens', label));
const saves = join(repo, 'logs/phone-lab/2026-11-26_231509/saves');
const scanPath = args.get('scan') ?? join(saves, readdirSync(saves).find((name) => /_scan_\d+\.bin$/.test(name)));
if (!existsSync(scanPath)) throw new Error(`scan file not found: ${scanPath}`);
mkdirSync(out, { recursive: true });

const bytes = readFileSync(scanPath);
const sha256 = createHash('sha256').update(bytes).digest('hex');
const server = await createServer({ root: webui, server: { port: 5189, strictPort: true, host: '127.0.0.1' }, logLevel: 'error', configFile: join(webui, 'vite.config.ts') });
await server.listen();
const browser = await chromium.launch({ args: ['--enable-webgl', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--force-color-profile=srgb'] });
const report = { label, scan: scanPath, scanSha256: sha256, viewport: '393x852@1', views: {} };
try {
  const page = await browser.newPage({ viewport: { width: 393, height: 852 }, deviceScaleFactor: 1 });
  page.on('pageerror', (error) => console.error('pageerror', error.message));
  await page.goto('http://127.0.0.1:5189/stress/scan-compare.html');
  await page.waitForFunction(() => window.__scanCompare !== undefined, null, { timeout: 120000 });
  await page.evaluate((base64) => window.__scanCompare.load(base64), bytes.toString('base64'));
  await page.locator('.scan-mesh-overlay').waitFor({ timeout: 120000 });
  await page.waitForFunction(() => document.querySelector('.scan-mesh-overlay')?.getAttribute('data-mesh-quality') !== 'pending', null, { timeout: 300000 });
  await page.waitForTimeout(1500);
  report.meshStatus = await page.locator('.scan-mesh-overlay').innerText();
  const canvas = page.locator('.scan-webgl-canvas');
  const box = await canvas.boundingBox();
  const drag = async (dx, dy) => {
    const x = box.x + box.width / 2; const y = box.y + box.height / 2;
    await page.mouse.move(x, y); await page.mouse.down();
    for (let step = 1; step <= 10; step += 1) await page.mouse.move(x + (dx * step) / 10, y + (dy * step) / 10);
    await page.mouse.up();
    await page.waitForTimeout(600);
  };
  const shot = async (name) => { await page.waitForTimeout(500); await canvas.screenshot({ path: join(out, `${name}.png`) }); report.views[name] = `${name}.png`; };
  const preset = async (name) => { await page.evaluate((value) => window.__scanCompare.preset(value), name); await page.waitForTimeout(900); };
  await preset('perspective'); await shot('3d');
  await preset('top'); await shot('top');
  await preset('front'); await shot('front');
  await preset('perspective'); await drag(-220, 0); await shot('side');
  await preset('perspective'); await drag(140, -90); await shot('angle-a');
  await preset('perspective'); await drag(-90, 110); await shot('angle-b');
  writeFileSync(join(out, 'report.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report));
} finally {
  await browser.close();
  await server.close();
}
