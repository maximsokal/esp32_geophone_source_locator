import { test as base, expect, type BrowserContext, type Page } from '@playwright/test';
import { loadFirmwarePages, type Producer } from './firmware-pages-producer.mjs';
import { fullDeviceStatus } from '../../console/fixtures/deviceStatusFull';
import { layoutIssues, assertReachable, contract } from '../helpers/layout';

export type PhoneInputs = { secure?: boolean; camera?: 'granted' | 'denied' | 'missing'; xr?: 'supported' | 'unsupported' | 'missing' | 'throws' };
declare global {
  interface Window { firmwarePhone: { cameraCalls: number; trackStops: number; xrCalls: string[] } }
}

export class FirmwarePages {
  readonly requests: { method: string; path: string; body: string | null }[] = [];
  readonly errors: string[] = [];
  readonly expectedErrors = new Set<string>();
  readonly status = fullDeviceStatus();
  statusMode: 'ok' | 'http' | 'malformed' = 'ok';
  seismicMode: 'ok' | 'http' | 'malformed' = 'ok';
  seismicSource = 'AD7606';
  wifiResponse = 'wifi-saved';
  setupResponse = 'wifi-form';
  verifyMode: 'ready' | 'incomplete' | 'http' | 'malformed' | 'offline' = 'ready';
  verifyHold?: Promise<void>;
  producer!: Producer;
  runPath = '';

  async install(context: BrowserContext, origin: string) {
    await context.setOffline(true);
    const { producer, bodies, assets, runPath } = await loadFirmwarePages();
    this.producer = producer; this.runPath = runPath;
    await context.routeWebSocket('**/*', socket => { this.errors.push(`Unexpected WebSocket: ${socket.url()}`); socket.close(); });
    await context.route('**/*', async route => {
      const request = route.request();
      const url = new URL(request.url());
      this.requests.push({ method: request.method(), path: url.pathname + url.search, body: request.postData() });
      if (url.origin !== origin) {
        this.errors.push(`Unexpected external request: ${request.url()}`); await route.abort(); return;
      }
      if (request.method() === 'POST' && url.pathname === '/api/setup/verify') {
        if (url.searchParams.get('t') !== '1234') {
          const expired = producer.cases.find(row => row.id === 'verify-report-expired')!;
          this.expectedErrors.add(url.pathname);
          await route.fulfill({ status: expired.status, contentType: expired.content_type, body: bodies[expired.id] }); return;
        }
        await this.verifyHold;
        if (this.verifyMode === 'offline') { this.expectedErrors.add(url.pathname); await route.abort('connectionreset'); return; }
        if (this.verifyMode === 'http') this.expectedErrors.add(url.pathname);
        const report = request.postDataJSON() as Record<string, unknown>;
        const mask = this.verifyMode === 'ready' ? ['secure_context', 'camera', 'xr_api', 'immersive_ar']
          .reduce((bits, key, index) => bits | (report[key] === true ? 1 << index : 0), 0) : 0;
        await route.fulfill({ status: this.verifyMode === 'http' ? 503 : 200, contentType: 'application/json',
          body: this.verifyMode === 'malformed' ? '{' : bodies[`verify-report-${mask}`] }); return;
      }
      if (request.method() === 'GET' && (url.pathname === '/api/status' || url.pathname === '/api/seismic')) {
        const mode = url.pathname === '/api/status' ? this.statusMode : this.seismicMode;
        if (mode === 'http') this.expectedErrors.add(url.pathname);
        const seismic = { source: this.seismicSource, sample_rate_hz: 500, count: 16, total_frames: 1600,
          channels: [1, 2, 3, 4].map(id => ({ id, samples_uv: [0, 10, -10, 20, -20, 5, -5, 0, 0, 10, -10, 20, -20, 5, -5, 0] })) };
        await route.fulfill({ status: mode === 'http' ? 503 : 200, contentType: 'application/json',
          body: mode === 'malformed' ? '{' : JSON.stringify(url.pathname === '/api/status' ? this.status : seismic) }); return;
      }
      if (request.method() === 'GET' && url.pathname.startsWith('/assets/')) {
        const name = url.pathname.slice('/assets/'.length);
        if (this.producer.assets[name]) {
          await route.fulfill({ contentType: name.endsWith('.css') ? 'text/css' : 'application/javascript', body: assets[name] }); return;
        }
      }
      const pages: Record<string, string> = { '/': 'fallback', '/seismic': 'seismic', '/setup': this.setupResponse,
        '/setup/ca-guide': 'ca-guide', '/setup/verify-ui': 'verify-ui', '/setup/ca-download': 'ca-unavailable',
        '/setup/ca': url.searchParams.get('installed') === '1' ? 'ca-confirmed' : 'ca-legacy',
        '/setup/ca.crt': 'ca-legacy-unavailable', '/setup/verify': url.searchParams.get('t') === '1234' ? 'verify-legacy' : 'verify-expired',
        '/saved': 'wifi-saved' };
      const id = request.method() === 'POST' && url.pathname === '/api/wifi/setup' ? this.wifiResponse :
        request.method() === 'GET' ? pages[url.pathname] : undefined;
      const captured = this.producer.cases.find(row => row.id === id);
      if (captured) {
        if (captured.status >= 400) this.expectedErrors.add(url.pathname);
        const headers: Record<string, string> = { 'Content-Type': captured.content_type };
        if (captured.cache_control) headers['Cache-Control'] = captured.cache_control;
        if (captured.location) headers.Location = captured.location;
        await route.fulfill({ status: captured.status, headers, body: bodies[captured.id] }); return;
      }
      if (request.method() === 'GET' && url.pathname === '/favicon.ico') { await route.fulfill({ status: 204 }); return; }
      this.errors.push(`Unexpected request: ${request.method()} ${url.pathname}`); await route.abort();
    });
  }
}

export async function openPhonePage(page: Page, path: string, inputs: PhoneInputs = {}) {
  await page.addInitScript(({ secure, camera = 'granted', xr = 'supported' }: PhoneInputs) => {
    window.firmwarePhone = { cameraCalls: 0, trackStops: 0, xrCalls: [] };
    if (secure !== undefined) Object.defineProperty(window, 'isSecureContext', { configurable: true, value: secure });
    Object.defineProperty(navigator, 'mediaDevices', { configurable: true, value: camera === 'missing' ? undefined : {
      async getUserMedia() {
        window.firmwarePhone.cameraCalls++;
        if (camera === 'denied') throw new DOMException('Permission denied', 'NotAllowedError');
        return { getTracks: () => [{ stop: () => { window.firmwarePhone.trackStops++; } }] };
      },
    } });
    Object.defineProperty(navigator, 'xr', { configurable: true, value: xr === 'missing' ? undefined : {
      async isSessionSupported(mode: string) {
        window.firmwarePhone.xrCalls.push(mode);
        if (xr === 'throws') throw new Error('Simulated XR capability failure');
        return xr === 'supported';
      },
    } });
  }, inputs);
  await page.goto(path);
  await page.evaluate(() => document.fonts.ready);
}

export async function firmwareLayoutIssues(page: Page) {
  const issues = await layoutIssues(page);
  issues.push(...await page.evaluate(({ controlMinPx, roundingPx: eps }) => {
    const errors: string[] = [];
    const width = document.documentElement.clientWidth;
    for (const el of document.querySelectorAll<HTMLElement>('a[href],summary,.header,.row,.trace-head,.meta-item,.title,.subtitle,.check,label,.meta-value,.value')) {
      if (!el.getClientRects().length || getComputedStyle(el).visibility === 'hidden') continue;
      const r = el.getBoundingClientRect();
      if (!el.closest('.table-wrap') && (r.left < -eps || r.right > width + eps || el.scrollWidth > el.clientWidth + eps)) {
        errors.push(`firmware bounds/text: ${el.tagName} ${el.textContent?.slice(0, 60)}`);
      }
      if (el.matches('a[href],summary') && (r.width < controlMinPx - eps || r.height < controlMinPx - eps)) {
        errors.push(`firmware target: ${el.textContent?.slice(0, 60)} actual=${r.width}x${r.height}`);
      }
    }
    for (const wrap of document.querySelectorAll<HTMLElement>('.table-wrap')) {
      const r = wrap.getBoundingClientRect();
      if (r.left < -eps || r.right > width + eps) errors.push('firmware table wrapper outside viewport');
      wrap.scrollLeft = wrap.scrollWidth;
      const last = wrap.querySelector('th:last-child')?.getBoundingClientRect();
      if (last && last.right > r.right + eps) errors.push('firmware last table column unreachable');
      wrap.scrollLeft = 0;
    }
    for (const parent of document.querySelectorAll('.header,.row,.trace-head,.meta,.grid,.pods')) {
      const boxes = [...parent.children].map(el => el.getBoundingClientRect()).filter(r => r.width && r.height);
      for (let i = 0; i < boxes.length; i++) for (let j = i + 1; j < boxes.length; j++) {
        const a = boxes[i], b = boxes[j];
        if (Math.min(a.right, b.right) - Math.max(a.left, b.left) > eps &&
            Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > eps) errors.push(`firmware sibling overlap: ${parent.className}`);
      }
    }
    return errors;
  }, contract.geometry));
  return issues;
}
export async function assertFirmwareLayout(page: Page) {
  const issues = await firmwareLayoutIssues(page);
  await base.info().attach('geometry', { contentType: 'application/json', body: JSON.stringify({ viewport: page.viewportSize(), issues }) });
  expect(issues, 'Firmware bounds, text, action sizes and overlap').toEqual([]);
  await assertReachable(page);
  for (const link of await page.locator('a[href]:visible').all()) {
    await link.scrollIntoViewIfNeeded();
    expect(await link.evaluate(el => { const r = el.getBoundingClientRect(); return el.contains(document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2)); })).toBe(true);
  }
}

export const test = base.extend<{ firmware: FirmwarePages }>({
  firmware: async ({ context, baseURL }, use) => {
    const firmware = new FirmwarePages(); await firmware.install(context, new URL(baseURL!).origin); await use(firmware);
  },
  page: async ({ page, firmware, browser }, use, testInfo) => {
    expect(await page.evaluate(() => navigator.onLine), 'Unrouted requests must have no network access').toBe(false);
    page.on('pageerror', error => firmware.errors.push(`Uncaught: ${error.message}`));
    page.on('console', message => {
      if (message.type() !== 'error') return;
      const url = message.location().url;
      if (!(url && firmware.expectedErrors.has(new URL(url).pathname) && message.text().startsWith('Failed to load resource:'))) {
        firmware.errors.push(`Console: ${message.text()}`);
      }
    });
    await page.clock.setFixedTime(new Date('2026-01-01T12:00:00Z'));
    await use(page);
    await testInfo.attach('runtime', { contentType: 'application/json', body: JSON.stringify({ browser: browser.version(),
      viewport: page.viewportSize(), locale: 'en (firmware pages have no locale catalog)', requests: firmware.requests,
      errors: firmware.errors, producer: { binary_sha256: firmware.producer.binary_sha256, inputs: firmware.producer.inputs } }) });
    await testInfo.attach('native-producer', { path: firmware.runPath, contentType: 'application/json' });
    expect(firmware.errors, 'No unexpected runtime or network errors').toEqual([]);
  },
});
export { expect };
