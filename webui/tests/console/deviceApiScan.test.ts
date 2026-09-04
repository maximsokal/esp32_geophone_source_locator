import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  fetchDeviceStatus,
  loadObjectScanArtifact,
  saveObjectScanArtifact,
} from '../../src/api/deviceApi';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('object-owned 3D scan transport', () => {
  it('uploads the artifact as exact opaque bytes to the selected object', async () => {
    const artifact = new Uint8Array([0x47, 0x58, 0x52, 0x53, 1, 2, 3, 255]);
    const fetchMock = vi.fn(async (_url: string, init?: RequestInit) => {
      const body = init?.body;
      expect(body).toBeInstanceOf(ArrayBuffer);
      expect(Array.from(new Uint8Array(body as ArrayBuffer))).toEqual(Array.from(artifact));
      return jsonResponse({ ok: true, action: 'scan_save', object_id: 'OBJ_0008', bytes: artifact.length });
    });
    vi.stubGlobal('fetch', fetchMock);

    const result = await saveObjectScanArtifact('OBJ_0008', artifact);

    expect(result.bytes).toBe(artifact.length);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0][0]).toBe('/api/control?scan=save&object_id=OBJ_0008');
    const init = fetchMock.mock.calls[0][1] as RequestInit;
    expect(init.method).toBe('POST');
    expect((init.headers as Record<string, string>)['Content-Type']).toBe('application/octet-stream');
  });

  it('round-trips loaded artifact bytes without JSON conversion', async () => {
    const artifact = new Uint8Array([1, 9, 3, 7, 5, 255]);
    const fetchMock = vi.fn(async () => new Response(artifact.slice().buffer, {
      status: 200,
      headers: { 'Content-Type': 'application/octet-stream' },
    }));
    vi.stubGlobal('fetch', fetchMock);

    const loaded = await loadObjectScanArtifact('OBJ_0008');

    expect(loaded).not.toBeNull();
    expect(Array.from(loaded ?? [])).toEqual(Array.from(artifact));
    expect(fetchMock.mock.calls[0][0]).toBe('/api/control?scan=load&object_id=OBJ_0008');
  });

  it('treats only HTTP 404 as no saved scan', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ error: 'ESP_ERR_NOT_FOUND' }, 404)));
    await expect(loadObjectScanArtifact('OBJ_0010')).resolves.toBeNull();
  });

  it('serializes scan upload ahead of a status poll to avoid a second ESP TLS request', async () => {
    let resolveSave!: (response: Response) => void;
    const fetchMock = vi.fn((url: string) => {
      if (url.includes('scan=save')) {
        return new Promise<Response>((resolve) => { resolveSave = resolve; });
      }
      return Promise.resolve(jsonResponse({ system: { state: 'NOT_READY' } }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const save = saveObjectScanArtifact('OBJ_0008', new Uint8Array([1, 2, 3]));
    const status = fetchDeviceStatus(false);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    resolveSave(jsonResponse({ ok: true, action: 'scan_save', object_id: 'OBJ_0008', bytes: 3 }));
    await save;
    await status;

    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls[1][0]).toBe('/api/status');
  });
});
