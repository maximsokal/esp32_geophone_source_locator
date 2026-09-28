import { afterEach, describe, expect, it, vi } from 'vitest';

import { fetchNrfLogs } from '../../src/api/deviceApi';
import type { NrfLogPage } from '../../src/domain/device';

function jsonResponse(body: unknown, status = 200, statusText = 'OK'): Response {
  return new Response(JSON.stringify(body), {
    status,
    statusText,
    headers: { 'Content-Type': 'application/json' },
  });
}

const page: NrfLogPage = {
  ok: true,
  node: 'remote2',
  channel: 2,
  records: [{
    token: 'opaque-00000001', boot_id: 7, seq: 1, uptime_ms: 100, esp_rx_uptime_ms: 112,
    event_id: 1, severity: 1, subsystem: 1, arg0: 0, arg1: 0,
  }],
  next_token: 'opaque-00000001',
  reset: false,
  counters: { overwritten_total: 2, lost_internal_total: 3, wire_gap_total: 4, local_drop_total: 5 },
  logs_unsupported: false,
  boot_id: 7,
};

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('fetchNrfLogs', () => {
  it('parses a valid log page and its counters', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse(page)));
    await expect(fetchNrfLogs('remote2')).resolves.toEqual({ ok: true, value: page });
  });

  it('returns server bad-request reasons as typed results', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ ok: false, reason: 'bad_limit' }, 400, 'Bad Request')));
    await expect(fetchNrfLogs('remote2', undefined, 500)).resolves.toEqual({
      ok: false,
      kind: 'bad_request',
      reason: 'bad_limit',
    });
  });

  it('passes an opaque cursor back verbatim without parsing it', async () => {
    const fetchMock = vi.fn(async () => jsonResponse(page));
    vi.stubGlobal('fetch', fetchMock);
    const token = 'opaque+token /?#&=value';

    await fetchNrfLogs('remote2', token, 17);

    const url = new URL(String(fetchMock.mock.calls[0][0]), 'http://device.test');
    expect(url.pathname).toBe('/api/nrf/logs');
    expect(url.searchParams.get('node')).toBe('remote2');
    expect(url.searchParams.get('after')).toBe(token);
    expect(url.searchParams.get('limit')).toBe('17');
  });

  it('returns malformed payloads and transport failures as network errors', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ ok: true, node: 'remote2', records: [] })));
    await expect(fetchNrfLogs('remote2')).resolves.toMatchObject({ ok: false, kind: 'network_error', error: 'invalid_response' });

    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ ...page, node: 'remote1', channel: 1 })));
    await expect(fetchNrfLogs('remote2')).resolves.toMatchObject({ ok: false, kind: 'network_error', error: 'invalid_response' });

    vi.stubGlobal('fetch', vi.fn(async () => { throw new Error('offline'); }));
    await expect(fetchNrfLogs('remote2')).resolves.toMatchObject({ ok: false, kind: 'network_error' });
  });
});
