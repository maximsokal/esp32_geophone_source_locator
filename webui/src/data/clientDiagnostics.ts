type ClientLogLevel = 'INFO' | 'WARN' | 'ERROR';

interface ClientLogEntry {
  level: ClientLogLevel;
  timeMs: number;
  message: string;
}

const ENDPOINT = '/api/client-log';
const FLUSH_INTERVAL_MS = 2500;
const MAX_BATCH_ENTRIES = 24;
const MAX_ENTRY_CHARS = 420;
const MAX_BATCH_CHARS = 3600;

let installed = false;
let queue: ClientLogEntry[] = [];
let flushTimer: number | null = null;
let flushing = false;

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
  if (queue.length > MAX_BATCH_ENTRIES * 2) queue = queue.slice(-MAX_BATCH_ENTRIES * 2);
  scheduleFlush(level === 'ERROR' ? 80 : FLUSH_INTERVAL_MS);
}

function scheduleFlush(delayMs: number): void {
  if (flushTimer !== null) return;
  flushTimer = window.setTimeout(() => {
    flushTimer = null;
    void flush();
  }, delayMs);
}

async function flush(): Promise<void> {
  if (flushing || queue.length === 0) return;
  flushing = true;
  const batch = queue.splice(0, MAX_BATCH_ENTRIES);
  let body = '';
  for (const entry of batch) {
    const line = `${entry.level}|${entry.timeMs}|${entry.message}\n`;
    if (body.length + line.length > MAX_BATCH_CHARS) break;
    body += line;
  }
  try {
    await fetch(ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      cache: 'no-store',
      credentials: 'same-origin',
      body,
    });
  } catch {
    // Do not recurse into console/error handlers if the ESP is temporarily busy.
  } finally {
    flushing = false;
    if (queue.length > 0) scheduleFlush(FLUSH_INTERVAL_MS);
  }
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
  window.addEventListener('pagehide', () => { void flush(); });

  enqueue('INFO', 'WEBUI_BOOT', {
    buildId: buildId(),
    href: location.href,
    userAgent: navigator.userAgent,
    secure: window.isSecureContext,
  });
  void clearOriginCachesBestEffort();
}
