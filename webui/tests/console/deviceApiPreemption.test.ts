import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  ApiRequestPreemptedError,
  DEVICE_API_FAST_TIMEOUT_MS,
  fetchDeviceStatus,
  sendControl,
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

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('ESP API read preemption', () => {
  it('starts a critical STOP ahead of queued normals when an active status GET hangs', async () => {
    vi.useFakeTimers();
    let statusSignal!: AbortSignal;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url === '/api/control' ? actionFrom(init) ?? url : url);
      if (url === '/api/status') {
        statusSignal = init?.signal as AbortSignal;
        return new Promise<Response>((_resolve, reject) => {
          statusSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        });
      }
      return Promise.resolve(jsonResponse({ ok: true, action: actionFrom(init) }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const status = fetchDeviceStatus(false);
    const statusReason = status.catch((reason: unknown) => reason);
    const normalOne = sendControl('activate_object', { object_id: 'OBJ_0001' });
    const normalTwo = sendControl('remove_object', { object_id: 'OBJ_0001' });
    const critical = sendControl('measurement_stop');

    expect(statusSignal.aborted).toBe(true);
    await vi.advanceTimersByTimeAsync(200);

    expect(starts).toEqual([
      '/api/status',
      'measurement_stop',
      'activate_object',
      'remove_object',
    ]);
    expect(DEVICE_API_FAST_TIMEOUT_MS).toBeGreaterThan(200);
    const reason = await statusReason;
    expect(reason).toBeInstanceOf(ApiRequestPreemptedError);
    expect(reason).toMatchObject({ name: 'ApiRequestPreemptedError', code: 'API_REQUEST_PREEMPTED' });
    await Promise.all([normalOne, normalTwo, critical]);

    fetchMock.mockImplementation(() => Promise.reject(new TypeError('offline')));
    const networkError = await fetchDeviceStatus(false, undefined, { preemptible: false }).catch((failure: unknown) => failure);
    expect(networkError).not.toBeInstanceOf(ApiRequestPreemptedError);
    expect(networkError).toMatchObject({ name: 'Error', message: expect.stringContaining('network error') });
  });

  it('lets preemption win while the response body is still being read', async () => {
    vi.useFakeTimers();
    let statusSignal!: AbortSignal;
    let bodyReadStarted!: () => void;
    const bodyStarted = new Promise<void>((resolve) => { bodyReadStarted = resolve; });
    const starts: string[] = [];
    const statusResponse = new Response();
    vi.spyOn(statusResponse, 'text').mockImplementation(() => {
      bodyReadStarted();
      return new Promise<string>((_resolve, reject) => {
        statusSignal.addEventListener('abort', () => reject(new Error('body aborted')), { once: true });
      });
    });
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url === '/api/control' ? actionFrom(init) ?? url : url);
      if (url === '/api/status') {
        statusSignal = init?.signal as AbortSignal;
        return Promise.resolve(statusResponse);
      }
      return Promise.resolve(jsonResponse({ ok: true, action: actionFrom(init) }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const status = fetchDeviceStatus(false);
    const statusReason = status.catch((reason: unknown) => reason);
    await bodyStarted;
    const critical = sendControl('acquisition_stop');
    await vi.advanceTimersByTimeAsync(100);

    expect(statusSignal.aborted).toBe(true);
    expect(starts).toEqual(['/api/status', 'acquisition_stop']);
    expect(await statusReason).toBeInstanceOf(ApiRequestPreemptedError);
    await critical;
  });

  it('does not abort an active mutation and still dispatches critical before queued normals', async () => {
    const mutationResponse = deferred<Response>();
    let mutationSignal!: AbortSignal;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      const action = actionFrom(init);
      starts.push(action ?? url);
      if (action === 'activate_object') {
        mutationSignal = init?.signal as AbortSignal;
        return mutationResponse.promise;
      }
      return Promise.resolve(jsonResponse({ ok: true, action }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const mutation = sendControl('activate_object', { object_id: 'OBJ_0001' });
    const normalOne = sendControl('remove_object', { object_id: 'OBJ_0001' });
    const normalTwo = sendControl('set_object_location', { object_id: 'OBJ_0001' });
    const normalThree = sendControl('set_object_cable_length', { object_id: 'OBJ_0001' });
    const critical = sendControl('measurement_stop');

    expect(mutationSignal.aborted).toBe(false);
    expect(starts).toEqual(['activate_object']);
    mutationResponse.resolve(jsonResponse({ ok: true, action: 'activate_object' }));

    await Promise.all([mutation, normalOne, normalTwo, normalThree, critical]);
    expect(mutationSignal.aborted).toBe(false);
    expect(starts).toEqual([
      'activate_object',
      'measurement_stop',
      'remove_object',
      'set_object_location',
      'set_object_cable_length',
    ]);
  });
});
