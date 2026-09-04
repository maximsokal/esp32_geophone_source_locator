type ClientLogLevel = 'INFO' | 'WARN' | 'ERROR';

interface ClientLogEntry {
  level: ClientLogLevel;
  timeMs: number;
  message: string;
}

const ENDPOINT = '/api/client-log';
const FLUSH_INTERVAL_MS = 5000;
const ERROR_FLUSH_DELAY_MS = 250;
const HEARTBEAT_INTERVAL_MS = 30000;
const REQUEST_TIMEOUT_MS = 2500;
const MAX_RETRY_BACKOFF_MS = 60000;
const MAX_BATCH_ENTRIES = 24;
const MAX_ENTRY_CHARS = 420;
const MAX_BATCH_CHARS = 3600;
const MAX_QUEUED_ENTRIES = MAX_BATCH_ENTRIES * 4;

let installed = false;
let queue: ClientLogEntry[] = [];
let flushTimer: number | null = null;
let heartbeatTimer: number | null = null;
let flushing = false;
let deliveredBatches = 0;
let failedBatches = 0;
let consecutiveFailures = 0;
let nextAllowedFlushMs = 0;

function safeString(value: unknown): string {
  if (value instanceof Error) return `${value.name}: ${value.message}`;
  if (typeof value === 'string') return value;
  try { return JSON.stringify(value); } catch { return String(value); }
}

function sanitize(value: string): string {
  return value.replace(/[\r\n\t]+/g, ' ').replace(/\s{2,}/g, ' ').slice(0, MAX_ENTRY_CHARS);
}

function buildId(): string {
  return document.querySelector<HTMLMetaElement>('meta[name="geolocator-webui-build"]')?.content ?? 'unknown';
}

function retryBackoffMs(): number {
  if (consecutiveFailures <= 0) return 0;
  return Math.min(MAX_RETRY_BACKOFF_MS, 5000 * (2 ** Math.min(consecutiveFailures - 1, 4)));
}

function enqueue(level: ClientLogLevel, message: string, details?: unknown): void {
  const suffix = details === undefined ? '' : ` | ${safeString(details)}`;
  queue.push({ level, timeMs: Math.round(performance.now()), message: sanitize(`${message}${suffix}`) });
  if (queue.length > MAX_QUEUED_ENTRIES) queue = queue.slice(-MAX_QUEUED_ENTRIES);
  scheduleFlush(level === 'ERROR' ? ERROR_FLUSH_DELAY_MS : FLUSH_INTERVAL_MS);
}

function scheduleFlush(delayMs: number): void {
  /* XR/domain modules also use diagnostics in Node-based unit tests. */
  if (typeof window === 'undefined') return;
  if (flushTimer !== null) return;
  const backoffRemaining = Math.max(0, nextAllowedFlushMs - performance.now());
  const delay = Math.max(delayMs, backoffRemaining);
  flushTimer = window.setTimeout(() => {
    flushTimer = null;
    void flush();
  }, delay);
}

function takeBatch(): { entries: ClientLogEntry[]; body: string } {
  const entries: ClientLogEntry[] = [];
  let body = '';
  for (let index = 0; index < queue.length && entries.length < MAX_BATCH_ENTRIES; index += 1) {
    const entry = queue[index];
    const line = `${entry.level}|${entry.timeMs}|${entry.message}\n`;
    if (body.length + line.length > MAX_BATCH_CHARS) break;
    entries.push(entry);
    body += line;
  }
  if (entries.length > 0) queue.splice(0, entries.length);
  return { entries, body };
}

function restoreBatch(entries: ClientLogEntry[]): void {
  if (entries.length === 0) return;
  queue = [...entries, ...queue].slice(0, MAX_QUEUED_ENTRIES);
}

async function flush(): Promise<void> {
  /*
   * XR/domain modules call diagnostics from Node-based unit tests. In that
   * environment there is deliberately no browser origin to POST to. Treat an
   * explicit flush exactly like scheduleFlush(): a no-op until a real browser
   * runtime exists. This guard must happen before taking a batch so test-only
   * calls cannot consume or mutate the browser delivery queue.
   */
  if (typeof window === 'undefined') return;

  if (flushing || queue.length === 0) return;
  if (performance.now() < nextAllowedFlushMs) {
    scheduleFlush(0);
    return;
  }

  const batch = takeBatch();
  if (batch.entries.length === 0 || batch.body.length === 0) return;

  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  flushing = true;
  try {
    const response = await fetch(ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      cache: 'no-store',
      credentials: 'same-origin',
      keepalive: true,
      signal: controller.signal,
      body: batch.body,
    });
    if (!response.ok) throw new Error(`client log HTTP ${response.status}`);
    deliveredBatches += 1;
    consecutiveFailures = 0;
    nextAllowedFlushMs = 0;
  }
  catch {
    failedBatches += 1;
    consecutiveFailures += 1;
    nextAllowedFlushMs = performance.now() + retryBackoffMs();
    restoreBatch(batch.entries);
  }
  finally {
    window.clearTimeout(timeout);
    flushing = false;
    if (queue.length > 0) scheduleFlush(consecutiveFailures > 0 ? retryBackoffMs() : FLUSH_INTERVAL_MS);
  }
}

function flushWithBeacon(): void {
  if (queue.length === 0 || typeof navigator.sendBeacon !== 'function') return;
  const batch = takeBatch();
  if (batch.entries.length === 0 || batch.body.length === 0) return;
  const payload = new Blob([batch.body], { type: 'text/plain; charset=utf-8' });
  if (!navigator.sendBeacon(ENDPOINT, payload)) restoreBatch(batch.entries);
}

async function clearOriginCachesBestEffort(): Promise<void> {
  let cacheCount = 0;
  try {
    if ('caches' in window) {
      const keys = await caches.keys();
      cacheCount = keys.length;
      await Promise.all(keys.map((key) => caches.delete(key)));
    }
  }
  catch (error) {
    enqueue('WARN', 'CACHE_STORAGE_CLEAR_FAILED', error);
  }

  let workerCount = 0;
  try {
    if ('serviceWorker' in navigator) {
      const registrations = await navigator.serviceWorker.getRegistrations();
      workerCount = registrations.length;
      await Promise.all(registrations.map((registration) => registration.unregister()));
    }
  }
  catch (error) {
    enqueue('WARN', 'SERVICE_WORKER_CLEAR_FAILED', error);
  }
  enqueue('INFO', 'CACHE_CLEANUP', { cacheCount, workerCount });
}

export const clientDiagnostics = {
  info(message: string, details?: unknown): void { enqueue('INFO', message, details); },
  warn(message: string, details?: unknown): void { enqueue('WARN', message, details); },
  error(message: string, details?: unknown): void { enqueue('ERROR', message, details); },
  flush,
};

export function installClientDiagnostics(): void {
  if (installed) return;
  installed = true;

  const nativeWarn = console.warn.bind(console);
  const nativeError = console.error.bind(console);
  console.warn = (...args: unknown[]): void => {
    enqueue('WARN', 'CONSOLE_WARN', args.map(safeString).join(' '));
    nativeWarn(...args);
  };
  console.error = (...args: unknown[]): void => {
    enqueue('ERROR', 'CONSOLE_ERROR', args.map(safeString).join(' '));
    nativeError(...args);
  };

  window.addEventListener('error', (event) => {
    enqueue('ERROR', 'WINDOW_ERROR', {
      message: event.message,
      file: event.filename,
      line: event.lineno,
      column: event.colno,
    });
  });
  window.addEventListener('unhandledrejection', (event) => {
    enqueue('ERROR', 'UNHANDLED_REJECTION', event.reason);
  });
  window.addEventListener('online', () => {
    enqueue('INFO', 'BROWSER_ONLINE');
    nextAllowedFlushMs = 0;
    void flush();
  });
  window.addEventListener('pagehide', flushWithBeacon);

  enqueue('INFO', 'WEBUI_BOOT', {
    buildId: buildId(),
    href: location.href,
    userAgent: navigator.userAgent,
    secure: window.isSecureContext,
  });

  /*
   * Diagnostics are secondary to the control plane. Do not open a competing
   * HTTPS POST during the page's initial status/assets burst. Failed delivery
   * backs off exponentially instead of retrying every 1.5 s forever.
   */
  void clearOriginCachesBestEffort();

  heartbeatTimer = window.setInterval(() => {
    if (document.visibilityState !== 'visible') return;
    enqueue('INFO', 'WEBUI_HEARTBEAT', {
      visibility: document.visibilityState,
      deliveredBatches,
      failedBatches,
      queued: queue.length,
      consecutiveFailures,
    });
  }, HEARTBEAT_INTERVAL_MS);

  window.addEventListener('beforeunload', () => {
    if (heartbeatTimer !== null) window.clearInterval(heartbeatTimer);
    heartbeatTimer = null;
  }, { once: true });
}
