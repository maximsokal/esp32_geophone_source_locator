// Whole-session replay of the production XR runtime in Chromium (stress/xr-replay.tsx), with main-thread time and renderer CPU time from the DevTools protocol.
//   node scripts/bench-xr-replay.mjs --label <name> [--packs <dir>] [--only a,b] [--scenarios scan,static] [--duration 30000] [--repeats 3] [--no-tags] [--no-seismic]
// Packs come from tests/bench/phase2/exportFrames.bench.ts. Run on a quiet machine: it measures time.
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { chromium } from '@playwright/test';
import { createServer } from 'vite';

const here = dirname(fileURLToPath(import.meta.url));
const webui = resolve(here, '..');
const repo = resolve(webui, '..');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 1) {
  if (!process.argv[i].startsWith('--')) continue;
  const next = process.argv[i + 1];
  args.set(process.argv[i].slice(2), next === undefined || next.startsWith('--') ? 'true' : next);
}
const label = args.get('label') ?? 'working-tree';
const packDir = resolve(args.get('packs') ?? join(repo, '.test-build/phase2/packs'));
const names = (args.get('only') ?? 'redwood-living1-noisy').split(',');
const scenarios = (args.get('scenarios') ?? 'scan').split(',');
const duration = Number(args.get('duration') ?? 30000);
const repeats = Number(args.get('repeats') ?? 1);
const options = { durationMs: duration, tags: args.get('no-tags') !== 'true', seismic: args.get('no-seismic') !== 'true' };
const out = join(repo, `.test-build/phase2/xr-replay-${label}.json`);

/*
 * Peak working set of the renderer process(es): the page AND all its workers (fusion, keeper, mesh, AprilTag) live in them. One helper samples every 500 ms
 * for the length of a run. Windows only; elsewhere the figure is reported as unknown.
 */
function watchMemory(pids) {
  if (process.platform !== 'win32' || pids.length === 0) return { stop: () => null };
  const script = `while ($true) { $sum = 0; foreach ($p in Get-Process -Id ${pids.join(',')} -ErrorAction SilentlyContinue) { $sum += $p.WorkingSet64 }; [Console]::Out.WriteLine($sum); Start-Sleep -Milliseconds 500 }`;
  const child = spawn('powershell', ['-NoProfile', '-NonInteractive', '-Command', script], { stdio: ['ignore', 'pipe', 'ignore'] });
  const samples = []; let pending = '';
  child.stdout.on('data', (chunk) => { pending += chunk.toString(); const lines = pending.split(/\r?\n/); pending = lines.pop(); for (const line of lines) { const value = Number(line); if (value > 0) samples.push(value / 1048576); } });
  return { stop: () => { child.kill(); return samples.length === 0 ? null : { samples: samples.length, first: samples[0], peak: Math.max(...samples), last: samples.at(-1) }; } };
}

const server = await createServer({
  root: webui, configFile: join(webui, 'vite.config.ts'), logLevel: 'error',
  server: { port: 5191, strictPort: true, host: '127.0.0.1', hmr: false, headers: { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' } },
});
await server.listen();
const browser = await chromium.launch({ args: ['--enable-precise-memory-info', '--enable-webgl', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
const report = { label, options, repeats, machine: process.env.COMPUTERNAME ?? 'unknown', browser: browser.version(), kind: 'DESKTOP Chromium with SwiftShader, not a phone', runs: [] };
let failed = 0;
try {
  const browserSession = await browser.newBrowserCDPSession();
  const cpuByType = async () => {
    const { processInfo } = await browserSession.send('SystemInfo.getProcessInfo');
    const sum = {};
    for (const process_ of processInfo) sum[process_.type] = (sum[process_.type] ?? 0) + process_.cpuTime;
    return sum;
  };
  for (const name of names) {
    for (const scenario of scenarios) {
      for (let repeat = 0; repeat < repeats; repeat += 1) {
        const context = await browser.newContext();
        const page = await context.newPage();
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        page.on('console', (message) => { if (message.type() === 'error') errors.push(message.text()); });
        try {
          await page.route(`**/${name}.pack`, (route) => route.fulfill({ body: readFileSync(join(packDir, `${name}.pack`)), contentType: 'application/octet-stream', headers: { 'Cross-Origin-Resource-Policy': 'same-origin' } }));
          await page.goto('http://127.0.0.1:5191/stress/xr-replay.html');
          await page.waitForFunction(() => document.getElementById('state')?.textContent === 'ready', null, { timeout: 120000 });
          const handle = await page.evaluateHandle((url) => window.__xrReplay.load(url), `/${name}.pack`);
          const session = await context.newCDPSession(page);
          await session.send('Performance.enable');
          const metrics = async () => Object.fromEntries((await session.send('Performance.getMetrics')).metrics.map((metric) => [metric.name, metric.value]));
          const { processInfo } = await browserSession.send('SystemInfo.getProcessInfo');
          const memory = watchMemory(processInfo.filter((process_) => process_.type === 'renderer').map((process_) => process_.id));
          const before = await metrics(); const cpuBefore = await cpuByType(); const wallStart = Date.now();
          let result; let workingSetMb;
          try { result = await page.evaluate(([pack, run]) => window.__xrReplay.run(pack, run), [handle, { ...options, scenario }]); }
          finally { workingSetMb = memory.stop(); }
          const after = await metrics(); const cpuAfter = await cpuByType(); const wallMs = Date.now() - wallStart;
          const cpu = Object.fromEntries(Object.keys(cpuAfter).map((type) => [type, (cpuAfter[type] - (cpuBefore[type] ?? 0)) * 1000]));
          const run = { pack: name, repeat, ...result, mainThreadTaskMs: (after.TaskDuration - before.TaskDuration) * 1000, mainThreadScriptMs: (after.ScriptDuration - before.ScriptDuration) * 1000, rendererCpuMs: cpu.renderer ?? null, allProcessCpuMs: cpu, wallMs, rendererWorkingSetMb: workingSetMb, errors };
          report.runs.push(run);
          console.log(`${name} ${scenario} #${repeat}: frameWork p50/p95/p99 ${run.frameWork.p50.toFixed(2)}/${run.frameWork.p95.toFixed(2)}/${run.frameWork.p99.toFixed(2)} ms | main task ${run.mainThreadTaskMs.toFixed(0)} ms (script ${run.mainThreadScriptMs.toFixed(0)}) | renderer CPU ${run.rendererCpuMs?.toFixed(0)} ms | integrated ${run.depth.integrated} kf ${run.depth.keyframes} pts ${run.finalPoints} | drop kf ${run.fusion?.droppedKeyframes ?? 'n/a'} lost ${run.fusion?.lostFrames ?? 'n/a'} q ${run.queueDepth.max} | mesh ${run.mesh.uploads} lat p95 ${run.mesh.depthToMeshMs.p95.toFixed(0)} | tag p50/p95 ${run.tags.latencyMs.p50.toFixed(0)}/${run.tags.latencyMs.p95.toFixed(0)} busy ${run.tags.busy} | seismic p95 ${run.seismic.intervalMs.p95.toFixed(0)} | finish ${run.finishMs?.toFixed(0) ?? '-'} | recall ${run.quality?.recall.toFixed(3) ?? '-'} precision ${run.quality?.precision.toFixed(3) ?? '-'} | heap peak ${run.heapMb.peak.toFixed(0)} MB, renderer working set peak ${run.rendererWorkingSetMb?.peak.toFixed(0) ?? '-'} MB | phase ${run.finalPhase} sessions ${run.liveSessions}${errors.length ? ` | ERRORS ${errors.slice(0, 3).join(' ; ').slice(0, 400)}` : ''}`);
        } catch (error) {
          failed += 1;
          console.error(`${name} ${scenario} #${repeat} FAILED: ${error.message.slice(0, 800)} ${errors.slice(0, 3).join(' ; ').slice(0, 600)}`);
        } finally {
          await context.close();
          mkdirSync(dirname(out), { recursive: true });
          writeFileSync(out, JSON.stringify(report, null, 2));
        }
      }
    }
  }
} finally {
  await browser.close();
  await server.close();
}
if (failed > 0) process.exit(1);
