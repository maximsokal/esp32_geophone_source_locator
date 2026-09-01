import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function freePort() {
  return await new Promise((resolve, reject) => {
    const server = createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      const port = typeof address === 'object' && address ? address.port : null;
      server.close((error) => {
        if (error) reject(error);
        else if (port == null) reject(new Error('Unable to allocate free port'));
        else resolve(port);
      });
    });
  });
}

const WEB_PORT = Number(process.env.MANUAL_UI_WEB_PORT ?? await freePort());
const CDP_PORT = Number(process.env.MANUAL_UI_CDP_PORT ?? await freePort());
const TARGET_URL = process.env.MANUAL_UI_URL
  ?? `http://127.0.0.1:${WEB_PORT}/stress/manual-placement-ui.html`;

function chromeExecutable() {
  const candidates = [
    process.env.CHROME_BIN,
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ].filter(Boolean);
  const found = candidates.find((path) => existsSync(path));
  if (!found) throw new Error(`Chrome/Chromium executable not found: ${candidates.join(', ')}`);
  return found;
}

async function waitForHttp(url, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url, { cache: 'no-store' });
      if (response.ok) return;
      lastError = new Error(`${response.status} ${response.statusText}`);
    } catch (error) {
      lastError = error;
    }
    await sleep(120);
  }
  throw new Error(`Timed out waiting for Vite at ${url}: ${String(lastError)}`);
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
    await sleep(120);
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
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('CDP websocket open timeout')), 10000);
      this.socket.addEventListener('open', () => {
        clearTimeout(timer);
        resolve();
      }, { once: true });
      this.socket.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error('CDP websocket error'));
      }, { once: true });
    });
    this.socket.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data));
      if (message.id !== undefined) {
        const pending = this.pending.get(message.id);
        if (!pending) return;
        this.pending.delete(message.id);
        clearTimeout(pending.timer);
        if (message.error) pending.reject(new Error(`${pending.method}: ${message.error.message}`));
        else pending.resolve(message.result ?? {});
        return;
      }
      for (const listener of this.listeners) listener(message);
    });
  }

  onEvent(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  send(method, params = {}, timeoutMs = 8000) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method}: CDP command timed out after ${timeoutMs} ms`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, method, timer });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  close() {
    this.socket.close();
  }
}

async function evaluate(client, expression) {
  const response = await client.send('Runtime.evaluate', {
    expression,
    returnByValue: true,
    awaitPromise: true,
  });
  if (response.exceptionDetails) {
    throw new Error(
      response.exceptionDetails.exception?.description
      ?? response.exceptionDetails.text
      ?? 'Runtime.evaluate failed',
    );
  }
  return response.result?.value;
}

async function waitUntil(client, expression, description, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await evaluate(client, expression)) return;
    await sleep(50);
  }
  throw new Error(`Timed out waiting for ${description}`);
}

function touchPoint(id, x, y) {
  return {
    id,
    x,
    y,
    radiusX: 6,
    radiusY: 6,
    rotationAngle: 0,
    force: 1,
  };
}

async function touchStart(client, rect, u, v) {
  const x = rect.left + rect.width * u;
  const y = rect.top + rect.height * v;
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchStart',
    touchPoints: [touchPoint(1, x, y)],
  });
}

async function touchEnd(client) {
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchEnd',
    touchPoints: [],
  });
}

async function stopChild(child) {
  if (!child) return;
  child.kill('SIGTERM');
  await Promise.race([
    new Promise((resolve) => child.once('exit', resolve)),
    sleep(1800),
  ]);
  if (child.exitCode === null) {
    child.kill('SIGKILL');
    await sleep(100);
  }
}

async function main() {
  if (typeof WebSocket !== 'function') throw new Error('Node 22+ WebSocket API required');

  const cwd = process.cwd();
  const executable = chromeExecutable();
  const profileDir = await mkdtemp(join(tmpdir(), 'geolocator-manual-e2e-'));
  const vite = spawn(
    process.execPath,
    [join(cwd, 'node_modules/vite/bin/vite.js'), '--host', '127.0.0.1', '--port', String(WEB_PORT), '--strictPort'],
    { cwd, stdio: ['ignore', 'pipe', 'pipe'] },
  );
  let viteStderr = '';
  vite.stderr.on('data', (chunk) => { viteStderr = (viteStderr + String(chunk)).slice(-12000); });

  const chrome = spawn(executable, [
    '--headless=new',
    '--no-sandbox',
    '--disable-dev-shm-usage',
    '--remote-allow-origins=*',
    `--remote-debugging-port=${CDP_PORT}`,
    `--user-data-dir=${profileDir}`,
    'about:blank',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  let chromeStderr = '';
  chrome.stderr.on('data', (chunk) => { chromeStderr = (chromeStderr + String(chunk)).slice(-12000); });

  let client = null;
  try {
    await waitForHttp(TARGET_URL);
    await waitForJson(`http://127.0.0.1:${CDP_PORT}/json/version`);
    const targetResponse = await fetch(
      `http://127.0.0.1:${CDP_PORT}/json/new?${encodeURIComponent(TARGET_URL)}`,
      { method: 'PUT' },
    );
    if (!targetResponse.ok) throw new Error(`Unable to create Chrome target: ${targetResponse.status}`);
    const target = await targetResponse.json();
    client = new CdpClient(target.webSocketDebuggerUrl);
    await client.open();

    const runtimeExceptions = [];
    const consoleErrors = [];
    client.onEvent((message) => {
      if (message.method === 'Runtime.exceptionThrown') {
        runtimeExceptions.push(
          message.params?.exceptionDetails?.exception?.description
          ?? message.params?.exceptionDetails?.text
          ?? 'runtime exception',
        );
      }
      if (message.method === 'Runtime.consoleAPICalled' && message.params?.type === 'error') {
        consoleErrors.push((message.params?.args ?? []).map((arg) => arg.value ?? arg.description ?? '').join(' '));
      }
    });

    await client.send('Runtime.enable');
    await client.send('Page.enable');
    await client.send('Emulation.setDeviceMetricsOverride', {
      width: 412,
      height: 915,
      deviceScaleFactor: 2.625,
      mobile: true,
      screenWidth: 412,
      screenHeight: 915,
    });
    await client.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
    await client.send('Emulation.setUserAgentOverride', {
      userAgent: 'Mozilla/5.0 (Linux; Android 14; SM-A546B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0 Mobile Safari/537.36',
      platform: 'Android',
    });
    await client.send('Page.navigate', { url: TARGET_URL });

    await waitUntil(
      client,
      'window.__MANUAL_PLACEMENT_UI_READY__ === true && !!document.getElementById("manual-placement-surface")',
      'manual placement fixture',
    );
    await sleep(150);

    const rect = await evaluate(client, `(() => {
      const r = document.getElementById('manual-placement-surface').getBoundingClientRect();
      return { left: r.left, top: r.top, width: r.width, height: r.height };
    })()`);
    if (!rect || rect.width < 200 || rect.height < 300) {
      throw new Error(`Invalid fixture rect: ${JSON.stringify(rect)}`);
    }

    // 1) Unsupported area: preview must say SCAN MORE and release must not create a pin.
    await touchStart(client, rect, 0.50, 0.50);
    await waitUntil(
      client,
      `(() => { const e = document.getElementById('manual-placement-surface'); return e.dataset.placementState === 'scan-more' && Number(e.dataset.pinCount) === 0; })()`,
      'SCAN MORE preview',
    );
    await touchEnd(client);
    await sleep(100);
    const afterScanMore = await evaluate(client, `(() => {
      const e = document.getElementById('manual-placement-surface');
      return { state: e.dataset.placementState, pins: Number(e.dataset.pinCount), lastMode: e.dataset.lastMode };
    })()`);
    if (afterScanMore.pins !== 0) throw new Error(`SCAN MORE created a pin: ${JSON.stringify(afterScanMore)}`);

    // 2) Two-plane region: lock must appear before release, but pin creation waits for release.
    await touchStart(client, rect, 0.25, 0.50);
    await waitUntil(
      client,
      `(() => { const e = document.getElementById('manual-placement-surface'); return e.dataset.placementState === 'edge' && Number(e.dataset.pinCount) === 0; })()`,
      'EDGE LOCK before release',
    );
    const edgeBeforeRelease = await evaluate(client, `(() => {
      const e = document.getElementById('manual-placement-surface');
      return { label: e.dataset.placementLabel, pins: Number(e.dataset.pinCount) };
    })()`);
    await touchEnd(client);
    await waitUntil(
      client,
      `(() => { const e = document.getElementById('manual-placement-surface'); return Number(e.dataset.pinCount) === 1 && e.dataset.lastMode === 'edge'; })()`,
      'edge pin creation on release',
    );

    // 3) Three-plane region: same release-only rule, now with a corner solution.
    await touchStart(client, rect, 0.75, 0.50);
    await waitUntil(
      client,
      `(() => { const e = document.getElementById('manual-placement-surface'); return e.dataset.placementState === 'corner' && Number(e.dataset.pinCount) === 1; })()`,
      'CORNER LOCK before release',
    );
    const cornerBeforeRelease = await evaluate(client, `(() => {
      const e = document.getElementById('manual-placement-surface');
      return { label: e.dataset.placementLabel, pins: Number(e.dataset.pinCount) };
    })()`);
    await touchEnd(client);
    await waitUntil(
      client,
      `(() => { const e = document.getElementById('manual-placement-surface'); return Number(e.dataset.pinCount) === 2 && e.dataset.lastMode === 'corner'; })()`,
      'corner pin creation on release',
    );

    const finalState = await evaluate(client, `(() => {
      const e = document.getElementById('manual-placement-surface');
      return {
        state: e.dataset.placementState,
        label: e.dataset.placementLabel,
        pins: Number(e.dataset.pinCount),
        lastMode: e.dataset.lastMode,
      };
    })()`);

    if (runtimeExceptions.length > 0) throw new Error(`Runtime exceptions: ${runtimeExceptions.join(' | ')}`);
    if (consoleErrors.length > 0) throw new Error(`Console errors: ${consoleErrors.join(' | ')}`);

    console.log('MANUAL_PLACEMENT_UI_E2E', JSON.stringify({
      pass: true,
      viewport: '412x915 @ 2.625 DPR',
      afterScanMore,
      edgeBeforeRelease,
      cornerBeforeRelease,
      finalState,
    }));
  } finally {
    try { client?.close(); } catch { /* best effort */ }
    await stopChild(chrome);
    await stopChild(vite);
    await rm(profileDir, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 }).catch(() => {});
    if (chrome.exitCode && chrome.exitCode !== 0) console.error(`Chrome stderr tail:\n${chromeStderr}`);
    if (vite.exitCode && vite.exitCode !== 0) console.error(`Vite stderr tail:\n${viteStderr}`);
  }
}

main()
  .then(() => process.exit(0))
  .catch((error) => {
    console.error(error?.stack ?? error);
    process.exit(1);
  });
