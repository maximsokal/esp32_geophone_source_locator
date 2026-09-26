import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  DEVICE_API_FAST_TIMEOUT_MS,
  deleteSessionRegistration,
  fetchDeviceStatus,
  saveObjectScanArtifact,
  sendControl,
} from '../../src/api/deviceApi';

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('ESP API request lanes', () => {
  it('runs critical before queued normal and bulk requests after preempting active bulk', async () => {
    let activeSignal: AbortSignal | null = null;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url);
      if (url.includes('scan=save') && activeSignal === null) {
        activeSignal = init?.signal as AbortSignal;
        return new Promise<Response>((_resolve, reject) => {
          activeSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        });
      }
      if (url.includes('scan=save')) return Promise.resolve(jsonResponse({ ok: true, action: 'scan_save' }));
      if (url === '/api/control') return Promise.resolve(jsonResponse({ ok: true, action: 'measurement_start' }));
      return Promise.resolve(jsonResponse({ system: { state: 'NOT_READY' } }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const activeBulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1]));
    const queuedNormal = fetchDeviceStatus(false);
    const queuedBulk = saveObjectScanArtifact('OBJ_0001', 'S000002', new Uint8Array([2]));
    const critical = sendControl('measurement_start');

    expect(activeSignal?.aborted).toBe(true);
    await expect(activeBulk).rejects.toMatchObject({ name: 'BulkTransferPreemptedError' });
    await Promise.all([critical, queuedNormal, queuedBulk]);
    expect(starts).toEqual([
      '/api/control?scan=save&object_id=OBJ_0001&session_id=S000001',
      '/api/control',
      '/api/status',
      '/api/control?scan=save&object_id=OBJ_0001&session_id=S000002',
    ]);
  });

  it('drains queued work in critical, normal, then bulk order after completion', async () => {
    const statusResponse = deferred<Response>();
    const starts: Array<{ url: string; action?: string }> = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      const action = typeof init?.body === 'string'
        ? (JSON.parse(init.body) as { action?: string }).action
        : undefined;
      starts.push({ url, action });
      if (url === '/api/status' && starts.length === 1) return statusResponse.promise;
      if (url.includes('scan=save')) return Promise.resolve(jsonResponse({ ok: true, action: 'scan_save' }));
      return Promise.resolve(jsonResponse({ ok: true, action }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const activeNormal = fetchDeviceStatus(false);
    const queuedNormal = sendControl('activate_object', { object_id: 'OBJ_0001' });
    const queuedBulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1]));
    const queuedCritical = sendControl('measurement_stop');
    statusResponse.resolve(jsonResponse({ system: { state: 'NOT_READY' } }));

    await Promise.all([activeNormal, queuedNormal, queuedBulk, queuedCritical]);
    expect(starts.map(({ url, action }) => action ?? (url.includes('scan=save') ? 'scan_save' : url))).toEqual([
      '/api/status', 'measurement_stop', 'activate_object', 'scan_save',
    ]);
  });

  it('starts the request timeout only when a queued request enters its lane', async () => {
    vi.useFakeTimers();
    const scanResponse = deferred<Response>();
    const statusResponse = deferred<Response>();
    let scanSignal!: AbortSignal;
    let statusSignal!: AbortSignal;
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      const signal = init?.signal as AbortSignal;
      signal.addEventListener('abort', () => undefined);
      if (url.includes('scan=save')) {
        scanSignal = signal;
        return scanResponse.promise;
      }
      statusSignal = signal;
      return statusResponse.promise;
    });
    vi.stubGlobal('fetch', fetchMock);

    const scan = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1]));
    const status = fetchDeviceStatus(false);
    await vi.advanceTimersByTimeAsync(DEVICE_API_FAST_TIMEOUT_MS + 1);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(scanSignal.aborted).toBe(false);

    scanResponse.resolve(jsonResponse({ ok: true, action: 'scan_save' }));
    await scan;
    expect(fetchMock).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(DEVICE_API_FAST_TIMEOUT_MS - 1);
    expect(statusSignal.aborted).toBe(false);

    statusResponse.resolve(jsonResponse({ system: { state: 'NOT_READY' } }));
    await status;
  });

  it('deletes a saved registration without nesting another queued request', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ ok: true, action: 'scan_delete' }));
    vi.stubGlobal('fetch', fetchMock);

    await expect(deleteSessionRegistration('OBJ_0001', 'S000001')).resolves.toMatchObject({ ok: true });
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0][0]).toBe('/api/control?scan=delete&object_id=OBJ_0001&session_id=S000001&kind=registration');
  });
});
