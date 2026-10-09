import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  ApiRequestOwnerRetiredError,
  createRequestOwner,
  fetchDeviceStatus,
  fetchLocalizationRuns,
  fetchNrfLogs,
  fetchSeismic,
  retireRequestOwner,
  sendControl,
} from '../../src/api/deviceApi';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((resolvePromise) => { resolve = resolvePromise; });
  return { promise, resolve };
}

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('ESP API request ownership', () => {
  it('removes queued reads, aborts an active read, rejects late completion, and preserves other owners', async () => {
    const urls: string[] = [];
    let activeSignal!: AbortSignal;
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      urls.push(url);
      if (url.startsWith('/api/seismic?')) {
        activeSignal = init?.signal as AbortSignal;
        return new Promise<Response>((_resolve, reject) => {
          activeSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        });
      }
      return Promise.resolve(jsonResponse({}));
    });
    vi.stubGlobal('fetch', fetchMock);

    const retiredOwner = createRequestOwner();
    const otherOwner = createRequestOwner();
    const activeRead = fetchSeismic({ owner: retiredOwner });
    const activeAssertion = expect(activeRead).rejects.toBeInstanceOf(ApiRequestOwnerRetiredError);
    const queuedRead = fetchNrfLogs('main', undefined, 50, { owner: retiredOwner });
    const queuedAssertion = expect(queuedRead).rejects.toBeInstanceOf(ApiRequestOwnerRetiredError);
    const otherOwnersRead = fetchLocalizationRuns('OBJ_0001', 'S000001', { owner: otherOwner });

    retireRequestOwner(retiredOwner);
    await Promise.all([activeAssertion, queuedAssertion, otherOwnersRead]);

    expect(activeSignal.aborted).toBe(true);
    expect(urls).toEqual([
      '/api/seismic?span_ms=440&max_points=220',
      '/api/localization/runs?object_id=OBJ_0001&session_id=S000001',
    ]);
  });

  it('does not abort or remove an owner mutation when its owner retires', async () => {
    let mutationSignal!: AbortSignal;
    let resolveMutation!: (response: Response) => void;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url);
      if (url === '/api/control') {
        const action = (JSON.parse(String(init?.body)) as { action: string }).action;
        if (action === 'activate_object') {
          mutationSignal = init?.signal as AbortSignal;
          return new Promise<Response>((resolve) => { resolveMutation = resolve; });
        }
        return Promise.resolve(jsonResponse({ ok: true, action }));
      }
      return Promise.resolve(jsonResponse({}));
    });
    vi.stubGlobal('fetch', fetchMock);

    const owner = createRequestOwner();
    const activeMutation = sendControl('activate_object', { object_id: 'OBJ_0001' }, { owner });
    const queuedMutation = sendControl('remove_object', { object_id: 'OBJ_0001' }, { owner });

    retireRequestOwner(owner);
    expect(mutationSignal.aborted).toBe(false);
    expect(starts).toEqual(['/api/control']);

    resolveMutation(jsonResponse({ ok: true, action: 'activate_object' }));
    await Promise.all([activeMutation, queuedMutation]);
    expect(mutationSignal.aborted).toBe(false);
    expect(starts).toEqual(['/api/control', '/api/control']);
  });

  it('coalesces identical status reads for the same owner', async () => {
    const response = deferred<Response>();
    const fetchMock = vi.fn((_url: string) => fetchMock.mock.calls.length === 1
      ? response.promise
      : Promise.resolve(jsonResponse(fullDeviceStatus())));
    vi.stubGlobal('fetch', fetchMock);

    const owner = createRequestOwner();
    const firstRead = fetchDeviceStatus(false, undefined, { owner, preemptible: true });
    const secondRead = fetchDeviceStatus(false, undefined, { owner, preemptible: true });
    const dispatchedBeforeResponse = fetchMock.mock.calls.length;
    response.resolve(jsonResponse(fullDeviceStatus()));

    const results = await Promise.all([firstRead, secondRead]);
    retireRequestOwner(owner);

    expect(dispatchedBeforeResponse).toBe(1);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(results).toHaveLength(2);
    expect(results[0]).toMatchObject({ system: { state: 'READY' } });
    expect(results[1]).toMatchObject({ system: { state: 'READY' } });
  });

  it('keeps identical status reads independent across owners during retirement', async () => {
    let firstSignal!: AbortSignal;
    let rejectFirst!: (reason: Error) => void;
    let secondSignal!: AbortSignal;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url);
      if (starts.length === 1) {
        firstSignal = init?.signal as AbortSignal;
        return new Promise<Response>((_resolve, reject) => { rejectFirst = reject; });
      }
      secondSignal = init?.signal as AbortSignal;
      return Promise.resolve(jsonResponse(fullDeviceStatus()));
    });
    vi.stubGlobal('fetch', fetchMock);

    const retiredOwner = createRequestOwner();
    const waitingOwner = createRequestOwner();
    const retiredRead = fetchDeviceStatus(false, undefined, { owner: retiredOwner, preemptible: true });
    const retiredAssertion = expect(retiredRead).rejects.toBeInstanceOf(ApiRequestOwnerRetiredError);
    const waitingRead = fetchDeviceStatus(false, undefined, { owner: waitingOwner, preemptible: true });

    retireRequestOwner(retiredOwner);
    rejectFirst(new Error('aborted'));
    await retiredAssertion;
    const waitingStatus = await waitingRead;

    expect(firstSignal.aborted).toBe(true);
    expect(secondSignal.aborted).toBe(false);
    expect(starts).toEqual(['/api/status', '/api/status']);
    expect(waitingStatus).toMatchObject({ system: { state: 'READY' } });
    retireRequestOwner(waitingOwner);
  });
});
