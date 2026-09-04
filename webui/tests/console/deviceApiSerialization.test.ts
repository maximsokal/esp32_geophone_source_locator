import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  fetchDeviceStatus,
  fetchSeismic,
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
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('serialized ESP API traffic', () => {
  it('does not start a control POST while a status fetch is still in flight', async () => {
    let resolveStatus!: (response: Response) => void;
    const fetchMock = vi.fn((url: string) => {
      if (url === '/api/status') {
        return new Promise<Response>((resolve) => { resolveStatus = resolve; });
      }
      return Promise.resolve(jsonResponse({ ok: true, action: 'activate_object' }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const status = fetchDeviceStatus(false);
    await Promise.resolve();
    expect(fetchMock).toHaveBeenCalledTimes(1);

    const control = sendControl('activate_object', { object_id: 'OBJ_0001' });
    await Promise.resolve();
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveStatus(jsonResponse({ system: { state: 'NOT_READY' } }));
    await status;
    await control;

    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls[1][0]).toBe('/api/control');
  });

  it('coalesces live seismic requests while one sample fetch is pending', async () => {
    let resolveSeismic!: (response: Response) => void;
    const fetchMock = vi.fn(() => new Promise<Response>((resolve) => { resolveSeismic = resolve; }));
    vi.stubGlobal('fetch', fetchMock);

    const first = fetchSeismic(220);
    const second = fetchSeismic(220);
    await Promise.resolve();

    expect(first).toBe(second);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveSeismic(jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 0,
      buffered_frames: 0,
      total_frames: 0,
      timestamps_ms: [],
      channels: [],
    }));
    await Promise.all([first, second]);
  });
});
