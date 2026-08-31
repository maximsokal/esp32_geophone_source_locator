type ClientLogLevel = 'INFO' | 'WARN' | 'ERROR';

interface ClientLogEntry {
  level: ClientLogLevel;
  timeMs: number;
  message: string;
}

const ENDPOINT = '/api/client-log';
const FLUSH_INTERVAL_MS = 2000;
const RETRY_INTERVAL_MS = 1500;
const HEARTBEAT_INTERVAL_MS = 15000;
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

function enqueue(level: ClientLogLevel, message: string, details?: unknown): void {
  const suffix = details === undefined ? '' : ` | ${safeString(details)}`;
  queue.push({ level, timeMs: Math.round(performance.now()), message: sanitize(`${message}${suffix}`) });
  if (queue.length > MAX_QUEUED_ENTRIES) queue = queue.slice(-MAX_QUEUED_ENTRIES);
  scheduleFlush(level === 'ERROR' ? 60 : FLUSH_INTERVAL_MS);
}

function scheduleFlush(delayMs: number): void {
  if (flushTimer !== null) return;
  flushTimer = window.setTimeout(() => {
    flushTimer = null;
    void flush();
  }, delayMs);
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
  if (flushing || queue.length === 0) return;
  const batch = takeBatch();
  if (batch.entries.length === 0 || batch.body.length === 0) return;

  flushing = true;
  try {
    const response = await fetch(ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      cache: 'no-store',
      credentials: 'same-origin',
      keepalive: true,
      body: batch.body,
    });
    if (!response.ok) throw new Error(`client log HTTP ${response.status}`);
    deliveredBatches += 1;
  } catch {
    failedBatches += 1;
    restoreBatch(batch.entries);
  } finally {
    flushing = false;
    if (queue.length > 0) scheduleFlush(failedBatches > 0 ? RETRY_INTERVAL_MS : FLUSH_INTERVAL_MS);
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
  } catch (error) {
    enqueue('WARN', 'CACHE_STORAGE_CLEAR_FAILED', error);
  }

  let workerCount = 0;
  try {
    if ('serviceWorker' in navigator) {
      const registrations = await navigator.serviceWorker.getRegistrations();
      workerCount = registrations.length;
      await Promise.all(registrations.map((registration) => registration.unregister()));
    }
  } catch (error) {
    enqueue('WARN', 'SERVICE_WORKER_CLEAR_FAILED', error);
  }
  enqueue('INFO', 'CACHE_CLEANUP', { cacheCount, workerCount });
  await flush();
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
    void flush();
  });
  window.addEventListener('pagehide', flushWithBeacon);

  enqueue('INFO', 'WEBUI_BOOT', {
    buildId: buildId(),
    href: location.href,
    userAgent: navigator.userAgent,
    secure: window.isSecureContext,
  });

  // Push the first batch immediately. This makes a successful browser->ESP log
  // path visible in UART before the operator even starts WebXR.
  void flush();
  void clearOriginCachesBestEffort();

  heartbeatTimer = window.setInterval(() => {
    enqueue('INFO', 'WEBUI_HEARTBEAT', {
      visibility: document.visibilityState,
      deliveredBatches,
      failedBatches,
      queued: queue.length,
    });
    void flush();
  }, HEARTBEAT_INTERVAL_MS);

  window.addEventListener('beforeunload', () => {
    if (heartbeatTimer !== null) window.clearInterval(heartbeatTimer);
    heartbeatTimer = null;
  }, { once: true });
}
