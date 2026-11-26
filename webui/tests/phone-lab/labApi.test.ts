import { mkdirSync, readFileSync, readdirSync, existsSync } from 'node:fs';
import { createServer, type Server } from 'node:http';
import { join, resolve } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';
// @ts-expect-error plain .mjs dev scripts without type declarations
import { createPhoneLab, peerGuard, sanitizeLabEvent } from '../../scripts/phone-lab/plugin.mjs';

const webuiRoot = resolve(import.meta.dirname, '../..');
const base = resolve(webuiRoot, '../.test-build/phone-lab-api-test');
const servers: Server[] = [];
let n = 0;

async function start() {
  const root = join(base, `run-${process.pid}-${n += 1}`);
  const runDir = join(root, 'run');
  const storeDir = join(root, 'store');
  mkdirSync(root, { recursive: true });
  const lab = createPhoneLab({ runDir, runId: 'test-run', storeDir, loadModule: (p: string) => import(resolve(webuiRoot, p.replace(/^\//, ''))) });
  const server = createServer((req, res) => { void lab.middleware(req, res, () => { res.writeHead(418); res.end('next'); }); });
  await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
  servers.push(server);
  const port = (server.address() as { port: number }).port;
  const call = (path: string, init: RequestInit = {}) => fetch(`http://127.0.0.1:${port}${path}`, init);
  return { lab, runDir, storeDir, call };
}
afterEach(async () => { while (servers.length > 0) { const s = servers.pop()!; s.closeAllConnections(); await new Promise<void>((r) => s.close(() => r())); } });

describe('phone lab fake device API', () => {
  it('serves the full device status in SCAN_CHOICE and drives SCAN_START/BACK with generation checks', async () => {
    const { call } = await start();
    const status = await (await call('/api/status')).json();
    expect(status.workflow.step).toBe('SCAN_CHOICE');
    const bad = await (await call('/api/control', { method: 'POST', body: JSON.stringify({ action: 'workflow_command', command: 'SCAN_START', generation: status.workflow.generation + 5 }) })).json();
    expect(bad.workflow).toBeUndefined();
    const ok = await (await call('/api/control', { method: 'POST', body: JSON.stringify({ action: 'workflow_command', command: 'SCAN_START', generation: status.workflow.generation }) })).json();
    expect(ok.workflow.step).toBe('SCANNING');
    expect((await (await call('/api/status')).json()).workflow.step).toBe('SCANNING');
  });

  it('completes a scan like the firmware: SCAN_COMPLETE needs a saved scan and moves SCANNING to RECORD_READY', async () => {
    const { call, runDir } = await start();
    const command = async (name: string) => {
      const status = await (await call('/api/status')).json();
      return (await call('/api/control', { method: 'POST', body: JSON.stringify({ action: 'workflow_command', command: name, generation: status.workflow.generation }) })).json();
    };
    expect((await command('SCAN_START')).workflow.step).toBe('SCANNING');
    expect(await command('SCAN_COMPLETE')).toEqual({ ok: false, reason: 'not_allowed' }); // nothing saved yet: the UI must not wait for it
    await call('/api/control?scan=save&object_id=OBJ_0001&session_id=staging', { method: 'POST', body: 'scan-bytes' });
    const done = await command('SCAN_COMPLETE');
    expect(done.ok).toBe(true);
    expect(done.workflow.step).toBe('RECORD_READY');
    expect(done.workflow.allowed_actions).toEqual(['START_RECORDING', 'MOVE_SENSORS', 'BACK']);
    expect((await (await call('/api/status')).json()).workflow.step).toBe('RECORD_READY');
    expect((await command('BACK')).workflow.step).toBe('SCAN_CHOICE');
    expect((await command('SCAN_SKIP')).workflow.step).toBe('RECORD_READY');
    expect(readFileSync(join(runDir, 'problems.log'), 'utf8')).toContain('SCAN_COMPLETE in step SCANNING (no saved scan)');
  });

  it('refuses stale generations and commands the model has no transition for, instead of pretending success', async () => {
    const { call, runDir } = await start();
    const status = await (await call('/api/status')).json();
    const send = async (command: string, generation: number) => (await call('/api/control', { method: 'POST', body: JSON.stringify({ action: 'workflow_command', command, generation }) })).json();
    expect(await send('SCAN_START', status.workflow.generation + 3)).toEqual({ ok: false, reason: 'stale' });
    expect(await send('START_RECORDING', status.workflow.generation)).toEqual({ ok: false, reason: 'not_allowed' });
    expect(readFileSync(join(runDir, 'problems.log'), 'utf8')).toContain('START_RECORDING in step SCAN_CHOICE');
    expect((await (await call('/api/status')).json()).workflow.step).toBe('SCAN_CHOICE');
  });

  it('reports available localization geometry and passes non-API paths to the next handler', async () => {
    const { call } = await start();
    const runs = await (await call('/api/localization/runs?object_id=OBJ_0001&session_id=S000001')).json();
    expect(runs.geometry.available).toBe(true);
    expect((await call('/index.html')).status).toBe(418);
  });

  it('round-trips scan artifacts byte-exact, derives the scan id, and keeps copies in the run folder', async () => {
    const { call, runDir, storeDir } = await start();
    const q = 'object_id=OBJ_0001&session_id=S000001';
    const payload = Buffer.from(Array.from({ length: 2048 }, (_, i) => (i * 13 + 5) & 0xff));
    expect((await call(`/api/control?scan=info&${q}`, { method: 'POST' })).status).toBe(200);
    expect(await (await call(`/api/control?scan=info&${q}`, { method: 'POST' })).json()).toMatchObject({ ok: false, error: 'ESP_ERR_NOT_FOUND' });
    expect((await call(`/api/control?scan=save&${q}`, { method: 'POST', body: payload })).status).toBe(200);
    const info = await (await call(`/api/control?scan=info&${q}`, { method: 'POST' })).json();
    expect(info.ok).toBe(true); expect(info.bytes).toBe(2048); expect(typeof info.id).toBe('string');
    expect(Buffer.from(await (await call(`/api/control?scan=load&${q}`, { method: 'POST' })).arrayBuffer()).equals(payload)).toBe(true);
    expect(readdirSync(join(runDir, 'saves')).some((f) => f.endsWith('_scan_2048.bin'))).toBe(true);
    expect(readdirSync(storeDir)).toEqual(['OBJ_0001__S000001__scan.bin']);
    await call(`/api/control?scan=delete&${q}`, { method: 'POST' });
    expect(readdirSync(storeDir)).toEqual([]);
  });

  it('stores registration artifacts under their own kind and rejects unsafe ids', async () => {
    const { call, storeDir } = await start();
    await call('/api/control?scan=save&object_id=OBJ_0001&session_id=S1&kind=registration', { method: 'POST', body: '{"schema":1}' });
    expect(readdirSync(storeDir)).toEqual(['OBJ_0001__S1__registration.bin']);
    for (const q of ['object_id=..%2F..%2Fx&session_id=S1', 'object_id=OBJ&session_id=S1&kind=evil', 'object_id=OBJ&session_id=S1&kind=occupancy&scan_id=..%5Cy']) {
      expect((await call(`/api/control?scan=save&${q}`, { method: 'POST', body: 'x' })).status).toBe(400);
    }
    expect(readdirSync(storeDir)).toEqual(['OBJ_0001__S1__registration.bin']);
  });

  it('stores the tag positions file under its own kind and round-trips it', async () => {
    const { call, storeDir } = await start();
    const q = 'object_id=OBJ_0001&session_id=staging&kind=tags';
    const body = '{"schema":1,"kind":"tag_positions"}';
    expect((await call(`/api/control?scan=save&${q}`, { method: 'POST', body })).status).toBe(200);
    expect(readdirSync(storeDir)).toEqual(['OBJ_0001__staging__tags.bin']);
    expect(await (await call(`/api/control?scan=info&${q}`, { method: 'POST' })).json()).toMatchObject({ ok: true, bytes: body.length, id: null });
    expect(await (await call(`/api/control?scan=load&${q}`, { method: 'POST' })).text()).toBe(body);
    await call(`/api/control?scan=delete&${q}`, { method: 'POST' });
    expect(readdirSync(storeDir)).toEqual([]);
  });

  it('accepts the real scan ids sent by the phone (scan-crc32:xxxxxxxx) for occupancy and preview artifacts', async () => {
    const { call, storeDir } = await start();
    const id = 'scan-crc32%3A732740d9'; // exactly what the scanner sent in the phone run that exposed this
    for (const kind of ['occupancy', 'preview']) {
      expect((await call(`/api/control?scan=save&object_id=OBJ_0001&session_id=staging&kind=${kind}&scan_id=${id}`, { method: 'POST', body: 'x' })).status).toBe(200);
    }
    expect(readdirSync(storeDir).sort()).toEqual(['OBJ_0001__staging__occupancy__scan-crc32_732740d9.bin', 'OBJ_0001__staging__preview__scan-crc32_732740d9.bin']);
    expect((await call('/api/control?scan=save&object_id=OBJ_0001&session_id=staging&kind=occupancy&scan_id=..%2F..%2Fx', { method: 'POST', body: 'x' })).status).toBe(400);
  });

  it('reports has_scan like the firmware: false with nothing saved, true once a scan is saved for the active object, false again after it is deleted (the Back button depends on it)', async () => {
    const { call } = await start();
    const hasScan = async () => (await (await call('/api/status')).json()).workflow.has_scan;
    expect(await hasScan()).toBe(false);
    await call('/api/control?scan=save&object_id=OBJ_0001&session_id=staging', { method: 'POST', body: 'scan-bytes' });
    expect(await hasScan()).toBe(true);
    await call('/api/control?scan=save&object_id=OBJ_0001&session_id=staging&kind=preview', { method: 'POST', body: 'preview-bytes' });
    expect(await hasScan(), 'a derivative alone is not a scan').toBe(true);
    await call('/api/control?scan=delete&object_id=OBJ_0001&session_id=staging', { method: 'POST' });
    expect(await hasScan()).toBe(false);
    await call('/api/control?scan=save&object_id=OBJ_0002&session_id=staging', { method: 'POST', body: 'other-object' });
    expect(await hasScan(), 'a scan of another object is not the scan of this object').toBe(false);
  });

  it('keeps bytes the phone captures (the real cloud of a rejected Continue) beside the run saves, under a safe name only', async () => {
    const { call, runDir } = await start();
    const bytes = new Uint8Array([1, 2, 3, 4, 5]);
    expect((await call('/lab/capture?name=continuation_b_rejected', { method: 'POST', body: bytes })).status).toBe(200);
    const saved = readdirSync(join(runDir, 'saves')).find((entry) => entry.includes('_continuation_b_rejected_5.bin'));
    expect(saved).toBeDefined();
    expect(Array.from(readFileSync(join(runDir, 'saves', saved!)))).toEqual([1, 2, 3, 4, 5]);
    expect(readFileSync(join(runDir, 'saves.log'), 'utf8')).toContain('capture continuation_b_rejected bytes=5');
    for (const name of ['../evil', 'a b', '', 'X'.repeat(41), 'Upper']) {
      expect((await call(`/lab/capture?name=${encodeURIComponent(name)}`, { method: 'POST', body: bytes })).status).toBe(400);
    }
  });

  it('records the production client-log lines with a receive time', async () => {
    const { call, runDir } = await start();
    expect((await call('/api/client-log', { method: 'POST', body: 'INFO|10|XR_TAG_CAPTURE_ENABLED\nWARN|11|XR_TAG_CAPTURE_REFUSED\n' })).status).toBe(204);
    const text = readFileSync(join(runDir, 'client-log.txt'), 'utf8');
    expect(text).toMatch(/XR_TAG_CAPTURE_ENABLED/);
    expect(text.trim().split('\n')).toHaveLength(2);
  });

  it('logs unmodeled API calls instead of silently inventing answers', async () => {
    const { call, runDir } = await start();
    expect((await call('/api/does-not-exist')).status).toBe(404);
    expect(readFileSync(join(runDir, 'problems.log'), 'utf8')).toContain('/api/does-not-exist');
  });

  it('refuses peers outside loopback and private ranges', async () => {
    const lab = createPhoneLab({ runDir: join(base, `peer-${process.pid}-${n += 1}`, 'run'), runId: 'x', storeDir: join(base, `peer-store-${process.pid}-${n}`), loadModule: async () => ({}) });
    const out: { code?: number } = {};
    const res = { statusCode: 0, writeHead(code: number) { out.code = code; }, end() {}, on() {} };
    await lab.middleware({ url: '/api/status', method: 'GET', socket: { remoteAddress: '8.8.8.8' } }, res, () => { throw new Error('must not pass through'); });
    expect(out.code).toBe(403);
    expect(lab.counts.rejectedPeers).toBe(1);
  });
});

describe('peer guard (applies to every path, not only /api and /lab)', () => {
  const run = (remoteAddress: string | undefined, url: string) => {
    const out: { code?: number; passed: boolean } = { passed: false };
    const res = { writeHead(code: number) { out.code = code; }, end() {} };
    peerGuard({ url, socket: { remoteAddress } }, res, () => { out.passed = true; });
    return out;
  };
  it('lets loopback and private peers through', () => {
    for (const peer of ['127.0.0.1', '::1', '192.168.0.50', '::ffff:10.1.2.3']) expect(run(peer, '/src/main.tsx').passed).toBe(true);
  });
  it('blocks public and unknown peers on app files and /@fs/ alike', () => {
    for (const peer of ['8.8.8.8', '::ffff:8.8.8.8', '2001:db8::1', undefined]) {
      for (const url of ['/', '/src/main.tsx', '/@fs/C:/anything', '/api/status']) {
        const out = run(peer, url);
        expect(out.passed).toBe(false);
        expect(out.code).toBe(403);
      }
    }
  });
});

describe('phone lab telemetry receiver', () => {
  const post = (call: (p: string, i?: RequestInit) => Promise<Response>, events: unknown[]) => call('/lab/events', { method: 'POST', body: JSON.stringify({ events }) });

  it('writes sanitized events and copies errors to browser-errors.log', async () => {
    const { call, runDir } = await start();
    const res = await (await post(call, [
      { type: 'DEVICE_INFO', seq: 0, t: 5, wall: 1, session: 'abc1', data: { secure: true } },
      { type: 'JS_ERROR', seq: 1, t: 6, wall: 2, session: 'abc1', data: { message: 'boom' } },
      { type: 'bad type' }, { type: 'X'.repeat(80) }, 'string', { type: 'OK_EVENT', data: [1] }, { type: 'BIG_EVENT', data: { x: 'y'.repeat(7000) } },
    ])).json();
    expect(res.accepted).toBe(2);
    const events = readFileSync(join(runDir, 'events.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l));
    expect(events.map((e) => e.type)).toEqual(['DEVICE_INFO', 'JS_ERROR']);
    expect(events[0]).toMatchObject({ runId: 'test-run', session: 'abc1', data: { secure: true } });
    expect(readFileSync(join(runDir, 'browser-errors.log'), 'utf8')).toContain('boom');
  });

  it('never lets event fields choose a path, and drops malformed sessions', () => {
    const event = sanitizeLabEvent({ type: 'LAB_MARK', session: '../../etc', data: { path: '../../x' } }, 'r');
    expect(event.session).toBeNull();
    expect(event.data.path).toBe('../../x'); // data is opaque text only; the receiver never uses it as a path
    expect(existsSync(resolve(base, '../x'))).toBe(false);
  });

  it('serves the telemetry client script', async () => {
    const { call } = await start();
    const res = await call('/lab/telemetry.js');
    expect(res.headers.get('content-type')).toContain('javascript');
    expect(await res.text()).toContain('XR_SESSION_STARTED');
  });
});
