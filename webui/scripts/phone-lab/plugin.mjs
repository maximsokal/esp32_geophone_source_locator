// Phone Lab dev-server middleware: a software stand-in for the ESP32 API the scanner flow needs, a log/telemetry receiver and
// the telemetry-script injection. Development only. Every file it writes lives under a directory it chose itself.
import { appendFileSync, existsSync, mkdirSync, readFileSync, readdirSync, unlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { isAllowedPeer } from './lan.mjs';

export const MAX_JSON_BYTES = 512 * 1024;
export const MAX_ARTIFACT_BYTES = 96 * 1024 * 1024;
const ID = /^[A-Za-z0-9_-]{1,64}$/;
// A scan id is a frame id such as `scan-crc32:732740d9` (the ':' must be accepted, then made filename-safe).
const SCAN_ID = /^[A-Za-z0-9_:-]{1,80}$/;
const KINDS = new Set(['scan', 'occupancy', 'preview', 'registration', 'tags']);
const TYPE = /^[A-Z][A-Z0-9_]{1,40}$/;

/**
 * The scan part of the firmware workflow (components/workflow_service/workflow_core.c): which command moves which step where,
 * and which actions each step offers. A command outside this table is refused as not_allowed (never silently accepted), so a
 * missing transition shows up as a visible failure and in problems.log instead of a UI that waits forever.
 */
export const WORKFLOW_ALLOWED = Object.freeze({
  SCAN_CHOICE: ['SCAN_START', 'SCAN_SKIP', 'BACK'],
  SCANNING: ['BACK'],
  RECORD_READY: ['START_RECORDING', 'MOVE_SENSORS', 'BACK'],
});
export const WORKFLOW_TRANSITIONS = Object.freeze({
  'SCAN_CHOICE:SCAN_START': 'SCANNING',
  'SCAN_CHOICE:SCAN_SKIP': 'RECORD_READY',
  'SCANNING:SCAN_COMPLETE': 'RECORD_READY',
  'SCANNING:BACK': 'SCAN_CHOICE',
  'RECORD_READY:BACK': 'SCAN_CHOICE',
});

export const PODS = [
  { pod: 0, node_id: 0, computed: true, assigned: true, leveled: true, computed_at_ms: 500 },
  { pod: 1, node_id: 1, computed: true, assigned: true, leveled: true, x_mm: 3000, y_mm: 0, z_mm: 0, computed_at_ms: 500 },
  { pod: 2, node_id: 2, computed: true, assigned: true, leveled: true, x_mm: 0, y_mm: 4000, z_mm: 0, computed_at_ms: 500 },
];

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = []; let size = 0;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > limit) { reject(Object.assign(new Error('body too large'), { statusCode: 413 })); req.destroy(); return; }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

export function sanitizeLabEvent(raw, runId) {
  if (raw === null || typeof raw !== 'object' || Array.isArray(raw) || typeof raw.type !== 'string' || !TYPE.test(raw.type)) return null;
  let data = {};
  if (raw.data !== undefined && raw.data !== null) {
    if (typeof raw.data !== 'object' || Array.isArray(raw.data)) return null;
    let text; try { text = JSON.stringify(raw.data); } catch { return null; }
    if (text.length > 6000) return null;
    data = JSON.parse(text);
  }
  const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
  return { runId, receivedAt: new Date().toISOString(), type: raw.type, seq: num(raw.seq), t: num(raw.t), wall: num(raw.wall),
    session: typeof raw.session === 'string' && /^[a-z0-9]{1,16}$/.test(raw.session) ? raw.session : null, data };
}

/**
 * runDir: per-run log directory. storeDir: persistent fake "device storage" (saved scans survive a server restart).
 * loadModule(path): loads a project TS module through Vite (fixtures, scan identity).
 */
/**
 * A new phone-lab run starts with an EMPTY fake device, like a freshly flashed board: the saved scans of the previous run used to stay in the store and the
 * app restored the last scan on the very first page of the next test (phone run 2026-11-26 22:37). Only the artifact files this lab writes are removed.
 * Set `keep` to carry the store over deliberately (PHONE_LAB_KEEP_STORE=1). Returns how many files were removed.
 */
export function resetDeviceStore(storeDir, keep = false) {
  if (keep || !existsSync(storeDir)) return 0;
  let removed = 0;
  for (const name of readdirSync(storeDir)) {
    if (!/^[A-Za-z0-9_-]+__[A-Za-z0-9_-]+__[a-z]+(__[A-Za-z0-9_-]+)?\.bin$/.test(name)) continue;
    unlinkSync(join(storeDir, name));
    removed += 1;
  }
  return removed;
}

export function createPhoneLab({ runDir, runId, storeDir, loadModule, now = () => new Date() }) {
  mkdirSync(runDir, { recursive: true });
  mkdirSync(storeDir, { recursive: true });
  mkdirSync(join(runDir, 'saves'), { recursive: true });
  const file = (name) => join(runDir, name);
  const counts = { api: 0, events: 0, clientLog: 0, unmodeled: 0, saves: 0, rejectedPeers: 0 };
  const stamp = () => now().toISOString();
  const problem = (message) => { counts.unmodeled += 1; appendFileSync(file('problems.log'), `${stamp()} ${message}\n`); };
  let status = null;
  let statusLoading = null;

  async function ensureStatus() {
    if (status !== null) return status;
    statusLoading ??= (async () => {
      const mod = await loadModule('/tests/console/fixtures/deviceStatusFull.ts');
      const next = mod.fullDeviceStatus();
      next.hardware = { required_missing: false, modules: [
        { id: 'ad7606', state: 'present', required: true, reason: 'ready', reason_code: 'ready' },
        { id: 'power_monitor', state: 'present', required: false, reason: 'sensor_ok', reason_code: 'sensor_ok' },
      ] };
      next.workflow.step = 'SCAN_CHOICE';
      next.workflow.allowed_actions = [...WORKFLOW_ALLOWED.SCAN_CHOICE];
      for (const pod of next.pods) pod.state = 'READY';
      status = next;
      return next;
    })();
    return statusLoading;
  }

  function artifactPath(objectId, sessionId, kind, scanId) {
    if (!ID.test(objectId) || !ID.test(sessionId) || !KINDS.has(kind) || (scanId !== null && !SCAN_ID.test(scanId))) return null;
    return join(storeDir, `${objectId}__${sessionId}__${kind}${scanId === null ? '' : `__${scanId.replace(/:/g, '_')}`}.bin`);
  }

  async function scanIdOf(bytes) {
    const mod = await loadModule('/src/xr/scanIdentity.ts');
    return mod.createScanIdentity(new Uint8Array(bytes)).frameId;
  }

  async function handleScan(req, res, url, send) {
    const q = url.searchParams;
    const action = q.get('scan');
    const kind = q.get('kind') ?? 'scan';
    const path = artifactPath(q.get('object_id') ?? '', q.get('session_id') ?? '', kind, q.get('scan_id'));
    if (path === null) return send(400, { ok: false, error: 'ESP_ERR_INVALID_ARG' });
    if (action === 'save') {
      const body = await readBody(req, MAX_ARTIFACT_BYTES);
      writeFileSync(path, body);
      const copy = `${stamp().replace(/[:.]/g, '-')}_${kind}_${body.length}.bin`;
      writeFileSync(join(runDir, 'saves', copy), body);
      counts.saves += 1;
      appendFileSync(file('saves.log'), `${stamp()} save ${q.get('object_id')}/${q.get('session_id')} kind=${kind} bytes=${body.length} copy=${copy}\n`);
      return send(200, { ok: true, action: kind });
    }
    if (action === 'info') {
      if (!existsSync(path)) return send(200, { ok: false, error: 'ESP_ERR_NOT_FOUND' });
      const bytes = readFileSync(path);
      if (kind === 'scan') return send(200, { ok: true, bytes: bytes.length, id: await scanIdOf(bytes) });
      if (kind === 'registration' || kind === 'tags') return send(200, { ok: true, bytes: bytes.length, id: null });
      const scanPath = artifactPath(q.get('object_id'), q.get('session_id'), 'scan', null);
      return send(200, { ok: true, bytes: bytes.length, source_id: scanPath !== null && existsSync(scanPath) ? await scanIdOf(readFileSync(scanPath)) : null });
    }
    if (action === 'load') {
      if (!existsSync(path)) return send(404, { ok: false, error: 'ESP_ERR_NOT_FOUND' });
      const bytes = readFileSync(path);
      res.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': bytes.length, 'cache-control': 'no-store' });
      return res.end(bytes);
    }
    if (action === 'delete') {
      if (existsSync(path)) unlinkSync(path);
      return send(200, { ok: true, action: 'delete' });
    }
    problem(`unmodeled scan action ${String(action)}`);
    return send(400, { ok: false, error: 'ESP_ERR_INVALID_ARG' });
  }

  async function handleApi(req, res, url, send) {
    const method = req.method ?? 'GET';
    const path = url.pathname;
    if (method === 'POST' && path === '/api/client-log') {
      const body = (await readBody(req, MAX_JSON_BYTES)).toString('utf8');
      appendFileSync(file('client-log.txt'), body.split('\n').filter(Boolean).map((line) => `${stamp()} ${line}`).join('\n') + '\n');
      counts.clientLog += 1;
      res.writeHead(204); return res.end();
    }
    const state = await ensureStatus();
    /* Like the firmware, the workflow reports whether a scan is saved for the active object (staged or in a session): the phone's Back and Continue
       decisions depend on it (phone runs of 2026-11-22 and 2026-11-26: the lab always said false, so Back sent the operator to "start a 3D scan?"). */
    state.workflow.has_scan = readdirSync(storeDir).some((name) => name.startsWith(`${state.workflow.object_id}__`) && name.endsWith('__scan.bin'));
    if (method === 'GET' && path === '/api/status') return send(200, state);
    if (method === 'GET' && path === '/api/localization/runs') {
      if (url.searchParams.get('job') === 'settings') return send(200, { ok: true, settings: null, presets: [], standard_processing: {} });
      if (url.searchParams.get('job') === 'status') return send(200, { ok: true, state: 'idle' });
      return send(200, { geometry: { available: true, pods: PODS }, cs_pairs: { available: false, pairs: [] }, picks: [], runs: [] });
    }
    if (method === 'GET' && path === '/api/nrf/logs') {
      return send(200, { ok: true, node: url.searchParams.get('node') ?? 'main', channel: null, records: [], next_token: null, reset: false,
        counters: { overwritten_total: 0, lost_internal_total: 0, wire_gap_total: 0, local_drop_total: 0 }, logs_unsupported: false, boot_id: null });
    }
    if (method === 'POST' && path === '/api/control') {
      if (url.searchParams.has('scan')) return handleScan(req, res, url, send);
      let body = {};
      try { body = JSON.parse((await readBody(req, MAX_JSON_BYTES)).toString('utf8') || '{}'); } catch { return send(400, { ok: false, error: 'bad json' }); }
      const wf = state.workflow;
      if (body.action === 'workflow_command') {
        if (body.generation !== wf.generation) return send(200, { ok: false, reason: 'stale' });
        const next = WORKFLOW_TRANSITIONS[`${wf.step}:${body.command}`];
        // Like the firmware: SCAN_COMPLETE needs a saved scan for the active object.
        const hasScan = readdirSync(storeDir).some((name) => name.endsWith('__scan.bin'));
        if (next === undefined || (body.command === 'SCAN_COMPLETE' && !hasScan)) {
          problem(`workflow command refused ${String(body.command)} in step ${wf.step}${next === undefined ? ' (no such transition modeled)' : ' (no saved scan)'}`);
          return send(200, { ok: false, reason: 'not_allowed' });
        }
        wf.step = next;
        wf.allowed_actions = [...(WORKFLOW_ALLOWED[next] ?? [])];
        wf.generation += 1;
        return send(200, { ok: true, workflow: wf });
      }
      problem(`unmodeled control action ${JSON.stringify(body).slice(0, 200)}`);
      return send(200, { ok: true, action: body.action ?? null });
    }
    problem(`unmodeled ${method} ${path}${url.search}`);
    return send(404, { ok: false, error: 'ESP_ERR_NOT_FOUND' });
  }

  async function handleLab(req, res, url, send) {
    if (req.method === 'POST' && url.pathname === '/lab/capture') {
      const name = url.searchParams.get('name') ?? '';
      if (!/^[a-z0-9_]{1,40}$/.test(name)) return send(400, { ok: false, error: 'ESP_ERR_INVALID_ARG' });
      const body = await readBody(req, MAX_ARTIFACT_BYTES);
      const copy = `${stamp().replace(/[:.]/g, '-')}_${name}_${body.length}.bin`;
      writeFileSync(join(runDir, 'saves', copy), body);
      counts.saves += 1;
      appendFileSync(file('saves.log'), `${stamp()} capture ${name} bytes=${body.length} copy=${copy}
`);
      return send(200, { ok: true, name });
    }
    if (req.method === 'POST' && url.pathname === '/lab/events') {
      let body;
      try { body = JSON.parse((await readBody(req, MAX_JSON_BYTES)).toString('utf8')); } catch { return send(400, { ok: false }); }
      const list = Array.isArray(body?.events) ? body.events.slice(0, 500) : [];
      let accepted = 0;
      for (const raw of list) {
        const event = sanitizeLabEvent(raw, runId);
        if (event === null) continue;
        appendFileSync(file('events.jsonl'), `${JSON.stringify(event)}\n`);
        if (event.type === 'JS_ERROR' || event.type === 'JS_REJECTION' || event.type === 'CONSOLE_ERROR') appendFileSync(file('browser-errors.log'), `${event.receivedAt} ${event.type} ${JSON.stringify(event.data)}\n`);
        accepted += 1; counts.events += 1;
      }
      return send(200, { ok: true, accepted });
    }
    if (req.method === 'GET' && url.pathname === '/lab/telemetry.js') {
      const body = readFileSync(new URL('./telemetry-client.js', import.meta.url));
      res.writeHead(200, { 'content-type': 'text/javascript; charset=utf-8', 'cache-control': 'no-store' });
      return res.end(body);
    }
    if (req.method === 'GET' && url.pathname === '/lab/health') return send(200, { ok: true, runId, counts });
    return send(404, { ok: false });
  }

  /** Connect-style middleware. Calls next() for everything that is not /api/* or /lab/*. */
  async function middleware(req, res, next) {
    const url = new URL(req.url ?? '/', 'http://phone-lab.local');
    const isApi = url.pathname.startsWith('/api/');
    const isLab = url.pathname.startsWith('/lab/');
    if (!isApi && !isLab) { next(); return; }
    if (!isAllowedPeer(req.socket.remoteAddress)) {
      counts.rejectedPeers += 1; res.writeHead(403); res.end('private network only'); return;
    }
    const started = Date.now();
    const send = (code, json) => { res.writeHead(code, { 'content-type': 'application/json', 'cache-control': 'no-store' }); res.end(JSON.stringify(json)); };
    res.on('finish', () => {
      if (url.pathname === '/lab/events' || url.pathname === '/api/client-log') return;
      counts.api += 1;
      appendFileSync(file('requests.jsonl'), `${JSON.stringify({ at: stamp(), method: req.method, path: url.pathname, query: url.search.slice(0, 200), status: res.statusCode, ms: Date.now() - started })}\n`);
    });
    try {
      await (isApi ? handleApi(req, res, url, send) : handleLab(req, res, url, send));
    } catch (error) {
      appendFileSync(file('server.log'), `${stamp()} handler error ${error instanceof Error ? error.stack : String(error)}\n`);
      if (!res.headersSent) send(error?.statusCode ?? 500, { ok: false, error: 'ESP_ERR_FAIL' });
    }
  }

  return { middleware, counts, storeFiles: () => readdirSync(storeDir) };
}

/** First middleware of the server: nothing (app files, /@fs/, API) is served to a peer outside loopback/private ranges. */
export function peerGuard(req, res, next) {
  if (isAllowedPeer(req.socket?.remoteAddress)) { next(); return; }
  res.writeHead(403, { 'content-type': 'text/plain' });
  res.end('private network only');
}

/** Vite plugin wrapper: peer guard and middleware first, telemetry script at the top of index.html. */
export function phoneLabPlugin(options) {
  return {
    name: 'geolocator-phone-lab',
    configureServer(server) {
      const lab = createPhoneLab({ ...options, loadModule: (path) => server.ssrLoadModule(path) });
      options.onReady?.(lab);
      server.middlewares.use(peerGuard);
      server.middlewares.use((req, res, next) => { void lab.middleware(req, res, next); });
    },
    transformIndexHtml() {
      return [{ tag: 'script', attrs: { src: '/lab/telemetry.js' }, injectTo: 'head-prepend' }];
    },
  };
}
