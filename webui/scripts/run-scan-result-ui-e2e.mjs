import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const PORT = Number(process.env.SCAN_UI_CDP_PORT ?? 9333);
const TARGET_URL = process.env.SCAN_UI_URL ?? 'http://127.0.0.1:4173/stress/scan-result-ui.html';

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function chromeExecutable() {
  const candidates = [
    process.env.CHROME_BIN,
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ].filter(Boolean);
  const found = candidates.find((path) => existsSync(path));
  if (!found) throw new Error(`Chrome/Chromium executable not found: ${candidates.join(', ')}`);
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
    await sleep(200);
  }
  throw new Error(`Timed out waiting for ${url}: ${String(lastError)}`);
}

class CdpClient {
  constructor(url) {
    this.socket = new WebSocket(url);
    this.nextId = 1;
    this.pending = new Map();
  }
  async open() {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('CDP websocket timeout')), 10000);
      this.socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, { once: true });
      this.socket.addEventListener('error', () => { clearTimeout(timer); reject(new Error('CDP websocket error')); }, { once: true });
    });
    this.socket.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data));
      if (message.id === undefined) return;
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(`${pending.method}: ${message.error.message}`));
      else pending.resolve(message.result ?? {});
    });
  }
  send(method, params = {}) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }
  close() { this.socket.close(); }
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

async function waitUntil(client, expression, description, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await evaluate(client, expression)) return;
    await sleep(100);
  }
  throw new Error(`Timed out waiting for ${description}`);
}

async function main() {
  if (typeof WebSocket !== 'function') throw new Error('Node 22+ WebSocket API required');
  const executable = chromeExecutable();
  const profileDir = await mkdtemp(join(tmpdir(), 'geolocator-result-e2e-'));
  const chrome = spawn(executable, [
    '--headless=new',
    '--no-sandbox',
    '--disable-dev-shm-usage',
    '--use-gl=angle',
    '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader',
    '--remote-allow-origins=*',
    `--remote-debugging-port=${PORT}`,
    `--user-data-dir=${profileDir}`,
    'about:blank',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let stderr = '';
  chrome.stderr.on('data', (chunk) => { stderr = (stderr + String(chunk)).slice(-20000); });
  let client = null;
  try {
    await waitForJson(`http://127.0.0.1:${PORT}/json/version`);
    const response = await fetch(`http://127.0.0.1:${PORT}/json/new?${encodeURIComponent(TARGET_URL)}`, { method: 'PUT' });
    if (!response.ok) throw new Error(`Unable to create target: ${response.status}`);
    const target = await response.json();
    client = new CdpClient(target.webSocketDebuggerUrl);
    await client.open();
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
    await client.send('Emulation.setUserAgentOverride', {
      userAgent: 'Mozilla/5.0 (Linux; Android 14; SM-A546B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0 Mobile Safari/537.36',
      platform: 'Android',
    });
    await client.send('Page.navigate', { url: TARGET_URL });
    await waitUntil(client, 'window.__SCAN_RESULT_FIXTURE_READY__ === true && !!document.querySelector("canvas")', 'fixture canvas');
    await sleep(700);

    const expected = {
      'mode-quality': ['quality', 'stable+tentative+pins+dims'],
      'mode-surface': ['surface', 'stable+pins+dims'],
      'mode-volume': ['search-volume', 'stable+free+unknown+pins+dims'],
      'mode-mesh': ['mesh', 'mesh+pins+dims'],
    };
    const modeResults = [];
    for (const [buttonId, [mode, fingerprint]] of Object.entries(expected)) {
      const result = await evaluate(client, `(() => {
        document.getElementById(${JSON.stringify(buttonId)}).click();
        const viewport = document.querySelector('[data-scan-mode]');
        return { mode: viewport?.dataset.scanMode, fingerprint: viewport?.dataset.layerFingerprint };
      })()`);
      await sleep(120);
      if (result.mode !== mode || result.fingerprint !== fingerprint) {
        throw new Error(`Mode ${buttonId} failed: ${JSON.stringify(result)} expected ${mode}/${fingerprint}`);
      }
      modeResults.push(result);
    }

    const beforeRotate = await evaluate(client, `Number(document.querySelector('[data-scan-mode]').dataset.cameraRevision || 0)`);
    await evaluate(client, `(() => {
      const canvas = document.querySelector('canvas');
      const r = canvas.getBoundingClientRect();
      const x = r.left + r.width * 0.46;
      const y = r.top + r.height * 0.48;
      const fire = (type, px, py, buttons) => canvas.dispatchEvent(new PointerEvent(type, {
        bubbles: true, cancelable: true, pointerId: 41, pointerType: 'touch', isPrimary: true,
        clientX: px, clientY: py, button: 0, buttons
      }));
      fire('pointerdown', x, y, 1);
      fire('pointermove', x + 70, y + 32, 1);
      fire('pointerup', x + 70, y + 32, 0);
      return true;
    })()`);
    await waitUntil(client, `Number(document.querySelector('[data-scan-mode]').dataset.cameraRevision || 0) > ${beforeRotate}`, 'one-finger orbit');

    const beforePinch = await evaluate(client, `(() => {
      const v = document.querySelector('[data-scan-mode]');
      return { revision: Number(v.dataset.cameraRevision || 0), distance: Number(v.dataset.cameraDistance || 0) };
    })()`);
    await evaluate(client, `(() => {
      const canvas = document.querySelector('canvas');
      const r = canvas.getBoundingClientRect();
      const cx = r.left + r.width * 0.5;
      const cy = r.top + r.height * 0.52;
      const fire = (type, id, x, y, primary, buttons) => canvas.dispatchEvent(new PointerEvent(type, {
        bubbles: true, cancelable: true, pointerId: id, pointerType: 'touch', isPrimary: primary,
        clientX: x, clientY: y, button: 0, buttons
      }));
      fire('pointerdown', 51, cx - 28, cy, true, 1);
      fire('pointerdown', 52, cx + 28, cy, false, 1);
      fire('pointermove', 51, cx - 70, cy - 8, true, 1);
      fire('pointermove', 52, cx + 70, cy + 8, false, 1);
      fire('pointerup', 51, cx - 70, cy - 8, true, 0);
      fire('pointerup', 52, cx + 70, cy + 8, false, 0);
      return true;
    })()`);
    await waitUntil(client, `Number(document.querySelector('[data-scan-mode]').dataset.cameraRevision || 0) > ${beforePinch.revision}`, 'two-finger zoom/pan');
    const afterPinch = await evaluate(client, `(() => {
      const v = document.querySelector('[data-scan-mode]');
      return { revision: Number(v.dataset.cameraRevision || 0), distance: Number(v.dataset.cameraDistance || 0) };
    })()`);
    if (!Number.isFinite(afterPinch.distance) || Math.abs(afterPinch.distance - beforePinch.distance) < 0.01) {
      throw new Error(`Pinch did not change camera distance: ${JSON.stringify({ beforePinch, afterPinch })}`);
    }

    const glError = await evaluate(client, `(() => {
      const canvas = document.querySelector('canvas');
      const gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
      return gl ? gl.getError() : -1;
    })()`);
    if (glError !== 0) throw new Error(`WebGL error after controls/mode switching: ${glError}`);

    console.log('SCAN_RESULT_UI_E2E', JSON.stringify({ pass: true, modeResults, beforeRotate, beforePinch, afterPinch }));
  } finally {
    try { client?.close(); } catch { /* best effort */ }
    chrome.kill('SIGTERM');
    await Promise.race([new Promise((resolve) => chrome.once('exit', resolve)), sleep(2500)]);
    if (chrome.exitCode === null) chrome.kill('SIGKILL');
    await rm(profileDir, { recursive: true, force: true, maxRetries: 4, retryDelay: 100 });
    if (chrome.exitCode && chrome.exitCode !== 0) console.error(stderr);
  }
}

main().catch((error) => {
  console.error(error?.stack ?? error);
  process.exitCode = 1;
});
