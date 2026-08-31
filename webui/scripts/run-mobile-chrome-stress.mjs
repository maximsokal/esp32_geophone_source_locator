import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const PORT = Number(process.env.XR_STRESS_CDP_PORT ?? 9222);
const TARGET_URL = process.env.XR_STRESS_URL ?? 'http://127.0.0.1:4173/stress/xr-mobile-stress.html';
const CPU_THROTTLE = Number(process.env.XR_STRESS_CPU_THROTTLE ?? 4);
const TIMEOUT_MS = Number(process.env.XR_STRESS_TIMEOUT_MS ?? 180000);
const PROFILE_CPU = process.env.XR_STRESS_PROFILE === '1';
const MOBILE_USER_AGENT = process.env.XR_STRESS_USER_AGENT ??
  'Mozilla/5.0 (Linux; Android 14; SM-A546B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Mobile Safari/537.36';

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function chromeExecutable() {
  const candidates = [
    process.env.CHROME_BIN,
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ].filter(Boolean);
  const found = candidates.find((path) => existsSync(path));
  if (!found) throw new Error(`Chrome/Chromium executable not found. Tried: ${candidates.join(', ')}`);
  return found;
}

async function waitForJson(url, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url, { cache: 'no-store' });
      if (response.ok) return await response.json();
      lastError = new Error(`${response.status} ${response.statusText}`);
    } catch (error) {
      lastError = error;
    }
    await sleep(250);
  }
  throw new Error(`Timed out waiting for ${url}: ${String(lastError)}`);
}

class CdpClient {
  constructor(url) {
    this.socket = new WebSocket(url);
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Set();
  }

  async open() {
    if (this.socket.readyState === WebSocket.OPEN) return;
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('CDP WebSocket open timeout')), 10000);
      this.socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, { once: true });
      this.socket.addEventListener('error', () => { clearTimeout(timer); reject(new Error('CDP WebSocket error')); }, { once: true });
    });
    this.socket.addEventListener('message', (event) => this.handleMessage(event));
  }

  handleMessage(event) {
    const message = JSON.parse(String(event.data));
    if (message.id !== undefined) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(`${pending.method}: ${message.error.message}`));
      else pending.resolve(message.result ?? {});
      return;
    }
    for (const listener of this.listeners) listener(message);
  }

  onEvent(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  send(method, params = {}) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  close() {
    this.socket.close();
  }
}

function remoteObjectText(arg) {
  if (arg.value !== undefined) return typeof arg.value === 'string' ? arg.value : JSON.stringify(arg.value);
  if (arg.description) return arg.description;
  return arg.type ?? 'unknown';
}

async function evaluate(client, expression) {
  const response = await client.send('Runtime.evaluate', {
    expression,
    returnByValue: true,
    awaitPromise: true,
  });
  if (response.exceptionDetails) {
    throw new Error(response.exceptionDetails.exception?.description ?? response.exceptionDetails.text ?? 'Runtime.evaluate failed');
  }
  return response.result?.value;
}

function summarizeCpuProfile(profile) {
  if (!profile || !Array.isArray(profile.nodes) || !Array.isArray(profile.samples)) return [];
  const nodes = new Map(profile.nodes.map((node) => [node.id, node]));
  const selfMicros = new Map();
  const deltas = Array.isArray(profile.timeDeltas) ? profile.timeDeltas : [];
  for (let index = 0; index < profile.samples.length; index += 1) {
    const nodeId = profile.samples[index];
    const delta = Number(deltas[index] ?? 0);
    if (!Number.isFinite(delta) || delta <= 0) continue;
    selfMicros.set(nodeId, (selfMicros.get(nodeId) ?? 0) + delta);
  }
  return [...selfMicros.entries()]
    .map(([nodeId, micros]) => {
      const node = nodes.get(nodeId);
      const frame = node?.callFrame ?? {};
      return {
        function: frame.functionName || '(anonymous)',
        selfMs: micros / 1000,
        url: frame.url || '',
        line: Number.isFinite(frame.lineNumber) ? frame.lineNumber + 1 : null,
      };
    })
    .filter((entry) => entry.function !== '(idle)' && entry.function !== '(program)')
    .sort((a, b) => b.selfMs - a.selfMs)
    .slice(0, 20);
}

async function main() {
  if (typeof WebSocket !== 'function') throw new Error('Node 22+ WebSocket API is required for the CDP runner.');

  const executable = chromeExecutable();
  const profileDir = await mkdtemp(join(tmpdir(), 'geolocator-xr-stress-'));
  const chrome = spawn(executable, [
    '--headless=new',
    '--no-sandbox',
    '--disable-dev-shm-usage',
    '--disable-gpu-watchdog',
    '--enable-precise-memory-info',
    '--remote-allow-origins=*',
    `--remote-debugging-port=${PORT}`,
    `--user-data-dir=${profileDir}`,
    '--js-flags=--max-old-space-size=384',
    'about:blank',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let chromeStderr = '';
  chrome.stderr.on('data', (chunk) => {
    chromeStderr += String(chunk);
    if (chromeStderr.length > 20000) chromeStderr = chromeStderr.slice(-20000);
  });

  let client = null;
  try {
    await waitForJson(`http://127.0.0.1:${PORT}/json/version`, 30000);
    const targetResponse = await fetch(
      `http://127.0.0.1:${PORT}/json/new?${encodeURIComponent(TARGET_URL)}`,
      { method: 'PUT' },
    );
    if (!targetResponse.ok) throw new Error(`Unable to create Chrome target: ${targetResponse.status} ${targetResponse.statusText}`);
    const target = await targetResponse.json();
    client = new CdpClient(target.webSocketDebuggerUrl);
    await client.open();

    const runtimeExceptions = [];
    const consoleErrors = [];
    client.onEvent((message) => {
      if (message.method === 'Runtime.exceptionThrown') {
        runtimeExceptions.push(message.params?.exceptionDetails?.exception?.description ?? message.params?.exceptionDetails?.text ?? 'runtime exception');
      }
      if (message.method === 'Runtime.consoleAPICalled') {
        const type = message.params?.type;
        const text = (message.params?.args ?? []).map(remoteObjectText).join(' ');
        if (text.includes('XR_STRESS_RESULT') || type === 'error' || type === 'warning') {
          console.log(`[chrome:${type}] ${text}`);
        }
        if (type === 'error') consoleErrors.push(text);
      }
    });

    const domains = [
      client.send('Page.enable'),
      client.send('Runtime.enable'),
      client.send('Performance.enable'),
    ];
    if (PROFILE_CPU) domains.push(client.send('Profiler.enable'));
    await Promise.all(domains);
    await client.send('Emulation.setCPUThrottlingRate', { rate: CPU_THROTTLE });
    await client.send('Emulation.setDeviceMetricsOverride', {
      width: 412,
      height: 915,
      deviceScaleFactor: 2.625,
      mobile: true,
      screenWidth: 412,
      screenHeight: 915,
      positionX: 0,
      positionY: 0,
      dontSetVisibleSize: false,
    });
    await client.send('Emulation.setUserAgentOverride', {
      userAgent: MOBILE_USER_AGENT,
      platform: 'Android',
    });

    if (PROFILE_CPU) {
      await client.send('Profiler.setSamplingInterval', { interval: 1000 });
      await client.send('Profiler.start');
    }

    await client.send('Page.navigate', { url: TARGET_URL });
    const deadline = Date.now() + TIMEOUT_MS;
    let done = false;
    while (Date.now() < deadline) {
      done = Boolean(await evaluate(client, 'window.__XR_STRESS_DONE__ === true'));
      if (done) break;
      await sleep(500);
    }
    if (!done) throw new Error(`Mobile Chrome stress test timed out after ${TIMEOUT_MS} ms`);

    const result = await evaluate(client, 'window.__XR_STRESS_RESULT__');
    const perf = await client.send('Performance.getMetrics');
    const heap = await client.send('Runtime.getHeapUsage');
    const profileResponse = PROFILE_CPU ? await client.send('Profiler.stop') : null;
    const cpuHotspots = summarizeCpuProfile(profileResponse?.profile);
    const metricMap = Object.fromEntries((perf.metrics ?? []).map((entry) => [entry.name, entry.value]));

    const browserSummary = {
      chromeExecutable: executable,
      emulation: {
        representativeDevice: 'mid-range Android proxy (Galaxy A54-class viewport/UA)',
        viewport: '412x915 @ 2.625 DPR',
        cpuThrottle: CPU_THROTTLE,
        v8OldSpaceLimitMiB: 384,
        cpuProfiler: PROFILE_CPU,
      },
      cdp: {
        taskDurationSeconds: metricMap.TaskDuration ?? null,
        scriptDurationSeconds: metricMap.ScriptDuration ?? null,
        layoutDurationSeconds: metricMap.LayoutDuration ?? null,
        recalcStyleDurationSeconds: metricMap.RecalcStyleDuration ?? null,
        jsHeapUsedBytes: heap.usedSize ?? null,
        jsHeapTotalBytes: heap.totalSize ?? null,
      },
      cpuHotspots,
      runtimeExceptions,
      consoleErrors,
    };

    console.log(PROFILE_CPU
      ? '=== MID-RANGE ANDROID CHROME XR CPU PROFILE ==='
      : '=== MID-RANGE ANDROID CHROME XR STRESS ===');
    console.log(JSON.stringify({ result, browserSummary }, null, 2));

    if (!PROFILE_CPU && (!result || result.pass !== true)) {
      throw new Error(`XR stress workload failed: ${JSON.stringify(result?.failures ?? ['missing result'])}`);
    }
    if (PROFILE_CPU && (!result || result.pass !== true)) {
      console.warn(`Profiling run exceeded hard-gate timing threshold; diagnostic profile is still valid: ${JSON.stringify(result?.failures ?? ['missing result'])}`);
    }
    if (runtimeExceptions.length > 0) {
      throw new Error(`Chrome runtime exceptions: ${runtimeExceptions.join(' | ')}`);
    }
    if (consoleErrors.length > 0) {
      throw new Error(`Chrome console errors: ${consoleErrors.join(' | ')}`);
    }
  } finally {
    try { client?.close(); } catch { /* best effort */ }
    chrome.kill('SIGTERM');
    await Promise.race([
      new Promise((resolve) => chrome.once('exit', resolve)),
      sleep(3000),
    ]);
    if (chrome.exitCode === null) chrome.kill('SIGKILL');
    await rm(profileDir, { recursive: true, force: true });
    if (chrome.exitCode && chrome.exitCode !== 0) {
      console.error('Chrome stderr tail:\n' + chromeStderr);
    }
  }
}

main().catch((error) => {
  console.error(error?.stack ?? error);
  process.exitCode = 1;
});
