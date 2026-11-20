// Development-only loopback log receiver for the Tag Lab. It owns every file path itself:
// nothing in a request body can select a filename or directory.
import { createServer } from 'node:http';
import { appendFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

export const EVENT_NAMES = Object.freeze([
  'LAB_START', 'CAMERA_PERMISSION', 'CAMERA_READY', 'CAMERA_FRAME', 'CONTRAST_CANDIDATE', 'DECODE_STARTED',
  'DECODE_FINISHED', 'TAG_DETECTED', 'TAG_POSE_ACCEPTED', 'TAG_POSE_REJECTED', 'TAG_CONFIRMED', 'TAG_LOST',
  'FRAME_DROPPED', 'WORKER_BUSY', 'WORKER_ERROR', 'READBACK_ERROR', 'PERFORMANCE_PRESSURE', 'SESSION_STOP', 'LAB_ERROR',
]);
const EVENT_SET = new Set(EVENT_NAMES);
const ERROR_EVENTS = new Set(['WORKER_ERROR', 'READBACK_ERROR', 'LAB_ERROR']);
export const MAX_BODY_BYTES = 256 * 1024;
export const MAX_EVENTS_PER_POST = 500;
const MAX_FIELDS_CHARS = 4000;
const SOURCES = new Set(['webcam', 'image', 'video', 'synthetic', 'lab']);

export function runStamp(now = new Date()) {
  const p = (n) => String(n).padStart(2, '0');
  return `${now.getFullYear()}-${p(now.getMonth() + 1)}-${p(now.getDate())}_${p(now.getHours())}${p(now.getMinutes())}${p(now.getSeconds())}`;
}

/** Returns a sanitized event or null. Only whitelisted names/sources; fields are a bounded JSON object. */
export function sanitizeEvent(raw, runId) {
  if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) return null;
  if (typeof raw.event !== 'string' || !EVENT_SET.has(raw.event)) return null;
  const source = typeof raw.source === 'string' && SOURCES.has(raw.source) ? raw.source : 'lab';
  const finite = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
  let fields = {};
  if (raw.fields !== undefined && raw.fields !== null) {
    if (typeof raw.fields !== 'object' || Array.isArray(raw.fields)) return null;
    let text;
    try { text = JSON.stringify(raw.fields); } catch { return null; }
    if (text.length > MAX_FIELDS_CHARS) return null;
    fields = JSON.parse(text);
  }
  return {
    runId,
    receivedAt: new Date().toISOString(),
    tMs: finite(raw.tMs),
    event: raw.event,
    source,
    sessionEpoch: finite(raw.sessionEpoch),
    frameId: finite(raw.frameId),
    fields,
  };
}

export function createTagLabLogServer({ logsRoot, port = 5189, host = '127.0.0.1', now = () => new Date() }) {
  const startedAt = now();
  const runId = runStamp(startedAt);
  const runDir = join(logsRoot, runId);
  mkdirSync(join(runDir, 'test-results'), { recursive: true });
  const eventsPath = join(runDir, 'events.jsonl');
  const errorsPath = join(runDir, 'browser-errors.log');
  const serverLogPath = join(runDir, 'server.log');
  const perfPath = join(runDir, 'performance.json');
  const metaPath = join(runDir, 'run-metadata.json');
  const counts = { events: 0, rejected: 0, errors: 0 };

  const serverLog = (message) => appendFileSync(serverLogPath, `${new Date().toISOString()} ${message}\n`);
  const writeMeta = (extra = {}) => writeFileSync(metaPath, JSON.stringify({
    runId, startedAt: startedAt.toISOString(), host, port, node: process.version, counts, ...extra,
  }, null, 2));
  writeMeta();
  serverLog(`start runDir=${runDir} bind=${host}:${port}`);

  function readBody(req, res) {
    return new Promise((resolve) => {
      const chunks = []; let size = 0; let done = false;
      req.on('data', (chunk) => {
        if (done) return;
        size += chunk.length;
        if (size > MAX_BODY_BYTES) { done = true; res.writeHead(413).end('too large'); resolve(null); req.destroy(); return; }
        chunks.push(chunk);
      });
      req.on('end', () => { if (!done) resolve(Buffer.concat(chunks).toString('utf8')); });
      req.on('error', () => resolve(null));
    });
  }

  const server = createServer(async (req, res) => {
    const url = (req.url ?? '').split('?')[0];
    const reply = (code, body = '') => { res.writeHead(code, { 'content-type': 'application/json', 'cache-control': 'no-store' }); res.end(JSON.stringify(body)); };
    try {
      if (req.method === 'GET' && url === '/health') return reply(200, { ok: true, runId, counts });
      if (req.method !== 'POST') return reply(405, { ok: false });
      if (url !== '/events' && url !== '/performance' && url !== '/stop') return reply(404, { ok: false });
      const text = await readBody(req, res);
      if (text === null) return undefined;
      let body;
      try { body = text === '' ? {} : JSON.parse(text); } catch { counts.rejected += 1; return reply(400, { ok: false, error: 'bad json' }); }
      if (url === '/events') {
        const list = Array.isArray(body?.events) ? body.events : null;
        if (list === null || list.length > MAX_EVENTS_PER_POST) { counts.rejected += 1; return reply(400, { ok: false, error: 'bad events' }); }
        let accepted = 0;
        for (const raw of list) {
          const event = sanitizeEvent(raw, runId);
          if (event === null) { counts.rejected += 1; continue; }
          appendFileSync(eventsPath, `${JSON.stringify(event)}\n`);
          if (ERROR_EVENTS.has(event.event)) { counts.errors += 1; appendFileSync(errorsPath, `${event.receivedAt} ${event.event} ${JSON.stringify(event.fields)}\n`); }
          accepted += 1; counts.events += 1;
        }
        return reply(200, { ok: true, accepted });
      }
      if (url === '/performance') {
        if (body === null || typeof body !== 'object' || Array.isArray(body)) return reply(400, { ok: false });
        const out = JSON.stringify({ runId, updatedAt: new Date().toISOString(), metrics: body }, null, 2);
        if (out.length > MAX_BODY_BYTES) return reply(413, { ok: false });
        writeFileSync(perfPath, out);
        return reply(200, { ok: true });
      }
      writeMeta({ stoppedAt: new Date().toISOString() });
      return reply(200, { ok: true });
    } catch (error) {
      serverLog(`handler error ${error instanceof Error ? error.message : String(error)}`);
      if (!res.headersSent) return reply(500, { ok: false });
      return undefined;
    }
  });

  return {
    runId, runDir, server,
    listen: () => new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(port, host, () => { serverLog(`listening ${host}:${server.address().port}`); resolve(server.address().port); });
    }),
    close: () => new Promise((resolve) => {
      writeMeta({ stoppedAt: new Date().toISOString() }); serverLog('close');
      server.close(() => resolve()); server.closeAllConnections?.();
    }),
    serverLog,
  };
}
