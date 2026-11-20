import { existsSync, mkdirSync, readFileSync, readdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';
// @ts-expect-error plain .mjs dev script without type declarations
import { createTagLabLogServer, EVENT_NAMES, MAX_BODY_BYTES, sanitizeEvent } from '../../scripts/tag-lab-log-server.mjs';

const base = resolve(import.meta.dirname, '../../../.test-build/tag-lab-logserver');
const created: { close: () => Promise<void> }[] = [];
let counter = 0;

async function start() {
  mkdirSync(base, { recursive: true });
  const root = join(base, `run-${process.pid}-${counter += 1}`);
  const lab = createTagLabLogServer({ logsRoot: root, port: 0 });
  const port = await lab.listen();
  created.push(lab);
  const post = (path: string, body: unknown, raw = false) => fetch(`http://127.0.0.1:${port}${path}`, { method: 'POST', body: raw ? (body as string) : JSON.stringify(body), headers: { 'content-type': 'application/json' } });
  return { lab, port, root, post };
}

afterEach(async () => { while (created.length > 0) await created.pop()!.close(); });

const event = (name: string, extra: Record<string, unknown> = {}) => ({ event: name, source: 'webcam', tMs: 12.5, sessionEpoch: 1, frameId: 7, fields: { a: 1 }, ...extra });

describe('tag lab log server', () => {
  it('creates a per-run directory with the documented layout and valid run metadata', async () => {
    const { lab } = await start();
    expect(existsSync(join(lab.runDir, 'test-results'))).toBe(true);
    expect(existsSync(join(lab.runDir, 'server.log'))).toBe(true);
    const meta = JSON.parse(readFileSync(join(lab.runDir, 'run-metadata.json'), 'utf8'));
    expect(meta.runId).toBe(lab.runId);
    expect(lab.runId).toMatch(/^\d{4}-\d{2}-\d{2}_\d{6}$/);
    expect(meta.host).toBe('127.0.0.1');
  });

  it('binds loopback only', async () => {
    const { lab } = await start();
    expect(lab.server.address().address).toBe('127.0.0.1');
  });

  it('appends whitelisted events as one valid JSON object per line', async () => {
    const { lab, post } = await start();
    const response = await post('/events', { events: [event('LAB_START'), event('TAG_DETECTED', { fields: { tagId: 2, node: 'G2' } })] });
    expect(response.status).toBe(200);
    const lines = readFileSync(join(lab.runDir, 'events.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l));
    expect(lines).toHaveLength(2);
    expect(lines[1]).toMatchObject({ runId: lab.runId, event: 'TAG_DETECTED', source: 'webcam', sessionEpoch: 1, frameId: 7, fields: { tagId: 2, node: 'G2' } });
    expect(typeof lines[1].receivedAt).toBe('string');
  });

  it('rejects unknown event names, bad shapes and oversize field blobs without writing them', async () => {
    const { lab, post } = await start();
    const huge = { x: 'y'.repeat(5000) };
    const response = await post('/events', { events: [event('NOT_AN_EVENT'), 'str', event('LAB_START', { fields: huge }), event('LAB_START', { fields: [1] }), event('LAB_START')] });
    expect(response.status).toBe(200);
    expect((await response.json()).accepted).toBe(1);
    expect(readFileSync(join(lab.runDir, 'events.jsonl'), 'utf8').trim().split('\n')).toHaveLength(1);
  });

  it('never lets request fields select a filesystem path', async () => {
    const { lab, post, root } = await start();
    await post('/events', { events: [event('LAB_START', { fields: { path: '../../evil.txt', file: 'C:\\evil.txt' }, source: '../../evil' })] });
    const entries = readdirSync(root);
    expect(entries).toEqual([lab.runId]);
    expect(existsSync(resolve(root, '..', 'evil.txt'))).toBe(false);
    const line = JSON.parse(readFileSync(join(lab.runDir, 'events.jsonl'), 'utf8').trim());
    expect(line.source).toBe('lab');
  });

  it('rejects malformed JSON, unknown routes and oversized bodies', async () => {
    const { post, port } = await start();
    expect((await post('/events', '{nope', true)).status).toBe(400);
    expect((await post('/other', {})).status).toBe(404);
    expect((await fetch(`http://127.0.0.1:${port}/events`)).status).toBe(405);
    const big = await post('/events', 'x'.repeat(MAX_BODY_BYTES + 10), true).catch(() => null);
    expect(big === null || big.status === 413).toBe(true);
  });

  it('copies error-class events into browser-errors.log', async () => {
    const { lab, post } = await start();
    await post('/events', { events: [event('WORKER_ERROR', { fields: { message: 'boom' } }), event('TAG_LOST')] });
    const errors = readFileSync(join(lab.runDir, 'browser-errors.log'), 'utf8');
    expect(errors).toContain('WORKER_ERROR');
    expect(errors).toContain('boom');
    expect(errors).not.toContain('TAG_LOST');
  });

  it('writes performance.json and records a stop in run-metadata.json', async () => {
    const { lab, post } = await start();
    expect((await post('/performance', { captureFps: 29.7 })).status).toBe(200);
    expect(JSON.parse(readFileSync(join(lab.runDir, 'performance.json'), 'utf8')).metrics.captureFps).toBe(29.7);
    await post('/stop', {});
    expect(JSON.parse(readFileSync(join(lab.runDir, 'run-metadata.json'), 'utf8')).stoppedAt).toBeTypeOf('string');
  });

  it('accepts exactly the 19 documented event types', () => {
    expect(EVENT_NAMES).toHaveLength(19);
    for (const name of EVENT_NAMES) expect(sanitizeEvent({ event: name }, 'r')).not.toBeNull();
    expect(sanitizeEvent({ event: 'CAMERA_FRAMES' }, 'r')).toBeNull();
  });
});
