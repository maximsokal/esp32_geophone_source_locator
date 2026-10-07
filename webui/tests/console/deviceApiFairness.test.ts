import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  NORMAL_BEFORE_BULK_LIMIT,
  saveObjectScanArtifact,
  sendControl,
  setNormalBeforeBulkLimitForTests,
} from '../../src/api/deviceApi';

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((resolvePromise) => { resolve = resolvePromise; });
  return { promise, resolve };
}

function actionFrom(init?: RequestInit): string | undefined {
  return typeof init?.body === 'string' ? (JSON.parse(init.body) as { action?: string }).action : undefined;
}

function requestName(url: string, init?: RequestInit): string {
  if (url.includes('scan=save')) return 'bulk';
  return actionFrom(init) ?? url;
}

afterEach(() => {
  setNormalBeforeBulkLimitForTests(NORMAL_BEFORE_BULK_LIMIT);
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('ESP API normal and bulk fairness', () => {
  it('dispatches a waiting bulk request after four normals, with critical work still first', async () => {
    expect(NORMAL_BEFORE_BULK_LIMIT).toBe(4);
    const activeCriticalResponse = deferred<Response>();
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(requestName(url, init));
      if (actionFrom(init) === 'measurement_start') return activeCriticalResponse.promise;
      return Promise.resolve(jsonResponse({ ok: true, action: actionFrom(init) ?? 'scan_save' }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const activeCritical = sendControl('measurement_start');
    const queuedCritical = sendControl('measurement_stop');
    const normals = [1, 2, 3, 4, 5].map((id) => sendControl('activate_object', { object_id: `OBJ_${id}` }));
    const bulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1]));

    activeCriticalResponse.resolve(jsonResponse({ ok: true, action: 'measurement_start' }));
    await Promise.all([activeCritical, queuedCritical, bulk, ...normals]);

    expect(starts).toEqual([
      'measurement_start',
      'measurement_stop',
      'activate_object',
      'activate_object',
      'activate_object',
      'activate_object',
      'bulk',
      'activate_object',
    ]);
  });

  it('honors a smaller limit and resets the counter when the normal lane empties', async () => {
    setNormalBeforeBulkLimitForTests(2);
    const firstGate = deferred<Response>();
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      const action = actionFrom(init);
      starts.push(requestName(url, init));
      if (action === 'measurement_start' && starts.length === 1) return firstGate.promise;
      return Promise.resolve(jsonResponse({ ok: true, action: action ?? 'scan_save' }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const gateOne = sendControl('measurement_start');
    const firstNormals = [1, 2].map((id) => sendControl('activate_object', { object_id: `OBJ_${id}` }));
    firstGate.resolve(jsonResponse({ ok: true, action: 'measurement_start' }));
    await Promise.all([gateOne, ...firstNormals]);

    const secondGate = deferred<Response>();
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      const action = actionFrom(init);
      starts.push(requestName(url, init));
      if (action === 'measurement_start') return secondGate.promise;
      return Promise.resolve(jsonResponse({ ok: true, action: action ?? 'scan_save' }));
    });
    const gateTwo = sendControl('measurement_start');
    const finalNormal = sendControl('remove_object', { object_id: 'OBJ_0001' });
    const bulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([2]));
    secondGate.resolve(jsonResponse({ ok: true, action: 'measurement_start' }));
    await Promise.all([gateTwo, finalNormal, bulk]);

    expect(starts).toEqual([
      'measurement_start',
      'activate_object',
      'activate_object',
      'measurement_start',
      'remove_object',
      'bulk',
    ]);
  });

  it('resets the fairness counter after a bulk request runs', async () => {
    setNormalBeforeBulkLimitForTests(2);
    const firstGate = deferred<Response>();
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      const action = actionFrom(init);
      starts.push(requestName(url, init));
      if (action === 'measurement_start' && starts.length === 1) return firstGate.promise;
      return Promise.resolve(jsonResponse({ ok: true, action: action ?? 'scan_save' }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const gateOne = sendControl('measurement_start');
    const normals = [1, 2, 3, 4].map((id) => sendControl('activate_object', { object_id: `OBJ_${id}` }));
    const firstBulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1]));
    const secondBulk = saveObjectScanArtifact('OBJ_0002', 'S000002', new Uint8Array([2]));
    firstGate.resolve(jsonResponse({ ok: true, action: 'measurement_start' }));
    await Promise.all([gateOne, firstBulk, secondBulk, ...normals]);

    expect(starts).toEqual([
      'measurement_start',
      'activate_object',
      'activate_object',
      'bulk',
      'activate_object',
      'activate_object',
      'bulk',
    ]);
  });
});
