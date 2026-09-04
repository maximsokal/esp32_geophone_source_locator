import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  DEVICE_API_FAST_TIMEOUT_MS,
  fetchDeviceStatus,
  sendControl,
} from '../../src/api/deviceApi';

function jsonResponse(body: unknown, status = 200, statusText = 'OK'): Response {
  return new Response(JSON.stringify(body), {
    status,
    statusText,
    headers: { 'Content-Type': 'application/json' },
  });
}

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('device API request pressure and failures', () => {
  it('coalesces identical concurrent fast status requests', async () => {
    let resolveFetch!: (response: Response) => void;
    const fetchMock = vi.fn(() => new Promise<Response>((resolve) => { resolveFetch = resolve; }));
    vi.stubGlobal('fetch', fetchMock);

    const first = fetchDeviceStatus(false);
    const second = fetchDeviceStatus(false);

    expect(first).toBe(second);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveFetch(jsonResponse({ system: { state: 'NOT_READY' } }));
    await Promise.all([first, second]);
  });

  it('releases single-flight state after a request settles', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ system: { state: 'NOT_READY' } }));
    vi.stubGlobal('fetch', fetchMock);

    await fetchDeviceStatus(false);
    await fetchDeviceStatus(false);

    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('lets a fast poll reuse an in-flight extended status response', async () => {
    let resolveFetch!: (response: Response) => void;
    const fetchMock = vi.fn(() => new Promise<Response>((resolve) => { resolveFetch = resolve; }));
    vi.stubGlobal('fetch', fetchMock);

    const extended = fetchDeviceStatus(true);
    const fast = fetchDeviceStatus(false);

    expect(fast).toBe(extended);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    resolveFetch(jsonResponse({ system: { state: 'NOT_READY' }, object_store: { available: true, objects: [] } }));
    await Promise.all([extended, fast]);
  });

  it('does not coalesce different extended status URLs', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ system: { state: 'NOT_READY' } }));
    vi.stubGlobal('fetch', fetchMock);

    await Promise.all([
      fetchDeviceStatus(true),
      fetchDeviceStatus(true, 'OBJ_0001'),
    ]);

    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls[0][0]).not.toBe(fetchMock.mock.calls[1][0]);
  });

  it('reports HTTP status, method, URL and firmware error together', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ error: 'ESP_ERR_NOT_FOUND' }, 404, 'Not Found'));
    vi.stubGlobal('fetch', fetchMock);

    await expect(fetchDeviceStatus(false)).rejects.toThrow(
      'ESP API 404 GET /api/status: ESP_ERR_NOT_FOUND',
    );
  });

  it('aborts a hung fast status request at the bounded timeout', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.fn((_url: string, init?: RequestInit) => new Promise<Response>((_resolve, reject) => {
      init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
    }));
    vi.stubGlobal('fetch', fetchMock);

    const request = fetchDeviceStatus(false);
    const assertion = expect(request).rejects.toThrow(
      `ESP API timeout (GET /api/status, ${DEVICE_API_FAST_TIMEOUT_MS} ms)`,
    );
    await vi.advanceTimersByTimeAsync(DEVICE_API_FAST_TIMEOUT_MS + 1);
    await assertion;
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it('never retries a failed control mutation', async () => {
    const fetchMock = vi.fn(async () => { throw new TypeError('socket reset'); });
    vi.stubGlobal('fetch', fetchMock);

    await expect(sendControl('set_object_cable_length', {
      object_id: 'OBJ_0001',
      pod_id: 1,
      length_m: 7.5,
    })).rejects.toThrow('ESP API network error (POST /api/control): socket reset');
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
});
