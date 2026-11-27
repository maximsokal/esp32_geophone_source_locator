// Main-thread cost of fusing depth frames synchronously vs handing them to the fusion worker, in Chromium with REAL Workers.
//   node scripts/bench-fusion-worker.mjs --label <name> [--repeats 5] [--pace 33] [--packs <dir>] [--only <pack>]
// Packs come from tests/bench/phase2/exportFrames.bench.ts. Variants alternate (sync, worker, sync, ...) after one warm-up pair per pack.
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
const repeats = Number(args.get('repeats') ?? 5);
const pace = Number(args.get('pace') ?? 33);
const packDir = resolve(args.get('packs') ?? join(repo, '.test-build/phase2/packs'));
const only = args.get('only');
const median = (values) => values.slice().sort((a, b) => a - b)[Math.floor(values.length / 2)];

const server = await createServer({
  root: webui, configFile: join(webui, 'vite.config.ts'), logLevel: 'error',
  server: { port: 5190, strictPort: true, host: '127.0.0.1', headers: { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' } },
});
await server.listen();
const browser = await chromium.launch({ args: ['--enable-precise-memory-info'] });
const report = { label, pace, repeats, machine: process.env.COMPUTERNAME ?? 'unknown', browser: browser.version(), kind: 'DESKTOP Chromium, not a phone', packs: {} };
try {
  const page = await browser.newPage();
  page.on('pageerror', (error) => console.error('pageerror', error.message));
  page.on('console', (message) => { if (message.type() === 'error') console.error('console.error', message.text()); });
  await page.goto('http://127.0.0.1:5190/stress/fusion-bench.html');
  await page.waitForFunction(() => document.getElementById('state')?.textContent === 'ready', null, { timeout: 120000 });
  console.log('isolated:', await page.evaluate(() => window.crossOriginIsolated));
  const packs = readdirSync(packDir).filter((name) => name.endsWith('.pack') && (only === undefined || name.startsWith(only)));
  for (const file of packs) {
    const name = file.replace(/\.pack$/, '');
    await page.route(`**/${file}`, (route) => route.fulfill({ body: readFileSync(join(packDir, file)), contentType: 'application/octet-stream', headers: { 'Cross-Origin-Resource-Policy': 'same-origin' } }));
    const handle = await page.evaluateHandle((url) => window.__fusionBench.load(url), `/${file}`);
    const run = (variant, paceMs) => page.evaluate(([pack, v, p]) => window.__fusionBench.run(pack, v, p), [handle, variant, paceMs]);
    await run('sync', 0); await run('worker', 0);                                 // warm-up pair
    const results = { sync: [], worker: [], workerFlood: [] };
    for (let r = 0; r < repeats; r += 1) { results.sync.push(await run('sync', pace)); results.worker.push(await run('worker', pace)); }
    for (let r = 0; r < Math.min(3, repeats); r += 1) results.workerFlood.push(await run('worker', 0));
    const pick = (list, f) => median(list.map(f));
    const agg = (list) => ({
      mainP50: pick(list, (x) => x.main.p50), mainP95: pick(list, (x) => x.main.p95), mainP99: pick(list, (x) => x.main.p99), mainMean: pick(list, (x) => x.main.mean), mainTotalMs: pick(list, (x) => x.mainMsTotal),
      workerCpuMs: pick(list, (x) => x.workerMsTotal), latencyP50: pick(list, (x) => x.latency.p50), latencyP95: pick(list, (x) => x.latency.p95), latencyP99: pick(list, (x) => x.latency.p99),
      finishMs: pick(list, (x) => x.finishMs), points: pick(list, (x) => x.points), promoted: pick(list, (x) => x.promoted), droppedOptional: pick(list, (x) => x.dropped),
      droppedKeyframes: pick(list, (x) => x.droppedKeyframes), maxQueue: pick(list, (x) => x.maxQueue), heapDeltaMb: pick(list, (x) => x.heapDeltaMb), frames: list[0].frames,
    });
    report.packs[name] = { sync: agg(results.sync), worker: agg(results.worker), workerFlood: agg(results.workerFlood) };
    const s = report.packs[name].sync; const w = report.packs[name].worker;
    console.log(`${name}: main p50 ${s.mainP50.toFixed(2)} -> ${w.mainP50.toFixed(3)} ms, p95 ${s.mainP95.toFixed(2)} -> ${w.mainP95.toFixed(3)} ms (${(100 * (1 - w.mainP95 / s.mainP95)).toFixed(0)}% less), worker ${w.workerCpuMs.toFixed(0)} ms, latency p95 ${w.latencyP95.toFixed(1)} ms, points ${s.points} vs ${w.points}, dropped ${w.droppedOptional}/${w.droppedKeyframes}`);
    await handle.dispose();
    await page.unroute(`**/${file}`);
  }
  const out = join(repo, `.test-build/phase2/fusion-browser-${label}.json`);
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, JSON.stringify(report, null, 2));
} finally {
  await browser.close();
  await server.close();
}
