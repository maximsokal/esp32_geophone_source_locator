import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  deleteObjectScanArtifact,
  fetchScanInfo,
  loadObjectScanArtifact,
  loadSessionPreviewArtifact,
  saveObjectScanArtifact,
  saveSessionOccupancyArtifact,
  saveSessionPreviewArtifact,
} from '../../src/api/deviceApi';
import {
  buildSessionOccupancyArtifact,
  decodeOccupancySolverArtifact,
  MAX_OCCUPANCY_ARTIFACT_BYTES,
  occupancyCrc32,
  occupancyFrameIdForScan,
} from '../../src/xr/occupancySolverArtifact';
import { encodeVoidScene, VOID_SCENE_REGISTRATION } from '../xr/support/voidScene';

/*
 * The empty (FREE) and filled (OCCUPIED) space of a saved scan travels to
 * the ESP32 as the compact occupancy artifact, stored beside the session's
 * scan (kind=occupancy). This virtual ESP32 mirrors the firmware store in
 * components/web_server/web_scan_store.c: one file per kind per session,
 * the occupancy size bound, 404 for a missing artifact, and deleting the
 * scan deletes the occupancy derived from it.
 */

const OCCUPANCY_MAX_BYTES = 1024 * 1024;
const PREVIEW_MAX_BYTES = 64 * 1024;

interface StoredRequest { method: string; path: string; contentType: string | undefined; bodyLength: number }

async function readBody(request: AsyncIterable<Uint8Array>): Promise<Uint8Array> {
  const chunks: Uint8Array[] = [];
  for await (const chunk of request) chunks.push(new Uint8Array(chunk));
  const body = new Uint8Array(chunks.reduce((total, chunk) => total + chunk.byteLength, 0));
  let offset = 0;
  for (const chunk of chunks) { body.set(chunk, offset); offset += chunk.byteLength; }
  return body;
}

async function startVirtualEsp32() {
  const { createServer } = await import('node:http');
  const files = new Map<string, Uint8Array>();
  const requests: StoredRequest[] = [];

  const server = createServer((request, response) => {
    void (async () => {
      const url = new URL(request.url ?? '/', 'http://esp32.local');
      const body = request.method === 'POST' ? await readBody(request) : new Uint8Array();
      requests.push({ method: request.method ?? '', path: `${url.pathname}${url.search}`, contentType: request.headers['content-type'], bodyLength: body.byteLength });
      const json = (status: number, value: unknown) => {
        const text = JSON.stringify(value);
        response.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
        response.end(text);
      };

      const action = url.searchParams.get('scan');
      if (request.method !== 'POST' || url.pathname !== '/api/control' || action === null) {
        json(404, { ok: false, error: 'route not found' });
        return;
      }
      const objectId = url.searchParams.get('object_id') ?? '';
      const sessionId = url.searchParams.get('session_id') ?? '';
      const kind = url.searchParams.get('kind') ?? 'scan';
      if (kind !== 'scan' && kind !== 'occupancy' && kind !== 'preview') {
        json(400, { ok: false, action: `scan_${action}`, error: 'ESP_ERR_INVALID_ARG' });
        return;
      }
      const fileName = kind === 'scan' ? 'scan-v1.bin' : kind === 'occupancy' ? 'occupancy-v1.bin' : 'preview-v1.bin';
      const file = `${objectId}/sessions/${sessionId}/${fileName}`;
      const occupancyFile = `${objectId}/sessions/${sessionId}/occupancy-v1.bin`;

      if (action === 'save') {
        if (body.byteLength === 0 || (kind === 'occupancy' && body.byteLength > OCCUPANCY_MAX_BYTES) || (kind === 'preview' && body.byteLength > PREVIEW_MAX_BYTES)) {
          json(400, { ok: false, action: 'scan_save', object_id: objectId, session_id: sessionId, error: 'ESP_ERR_INVALID_SIZE' });
          return;
        }
        files.set(file, body);
        json(200, { ok: true, action: 'scan_save', object_id: objectId, session_id: sessionId, bytes: body.byteLength });
        return;
      }
      if (action === 'load') {
        const stored = files.get(file);
        if (stored === undefined) {
          json(404, { ok: false, action: 'scan_load', error: 'ESP_ERR_NOT_FOUND' });
          return;
        }
        response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': stored.byteLength });
        response.end(Buffer.from(stored));
        return;
      }
      if (action === 'info') {
        const stored = files.get(file);
        if (stored === undefined) json(404, { ok: false, action: 'scan_info', error: 'ESP_ERR_NOT_FOUND' });
        else json(200, { ok: true, action: 'scan_info', object_id: objectId, session_id: sessionId, bytes: stored.byteLength });
        return;
      }
      if (action === 'delete') {
        files.delete(file);
        // The occupancy is derived from the scan: never leave it behind.
        if (kind === 'scan') {
          files.delete(occupancyFile);
          files.delete(`${objectId}/sessions/${sessionId}/preview-v1.bin`);
        }
        json(200, { ok: true, action: 'scan_delete', object_id: objectId, session_id: sessionId });
        return;
      }
      json(400, { ok: false, error: 'ESP_ERR_INVALID_ARG' });
    })();
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (address === null || typeof address === 'string') throw new Error('virtual ESP32 has no TCP port');
  const baseUrl = `http://127.0.0.1:${address.port}`;
  const nativeFetch = globalThis.fetch.bind(globalThis);
  vi.stubGlobal('fetch', (input: RequestInfo | URL, init?: RequestInit) => {
    const raw = typeof input === 'string' ? input : input instanceof URL ? input.toString() : input.url;
    return nativeFetch(new URL(raw, baseUrl), init);
  });
  return {
    files,
    requests,
    close: () => new Promise<void>((resolve) => server.close(() => resolve())),
  };
}

/* A small saved-scan snapshot: 0.18 m cells, two FREE, one OCCUPIED. */
function scanSnapshot() {
  return {
    freeAirPositions: new Float32Array([0.09, 0.09, 0.27, 0.27, 0.09, 0.27]),
    occupiedSurfacePositions: new Float32Array([0.09, 0.09, -0.09]),
    occupancyEnvelopeBounds: { min: [0, 0, -0.18] as [number, number, number], max: [0.36, 0.36, 0.36] as [number, number, number] },
    occupancySolverCandidateBelowSurfaceMeters: 1.5,
    occupancyCellsSaturated: false,
    occupancyFineVoxelSizeMeters: 0.18,
    occupancyVoxelSizeMeters: 0.18,
  };
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('3D scan empty/filled sectors -> ESP32', () => {
  it('stores the occupancy beside the scan, bound to that exact scan, without overwriting it', async () => {
    const esp = await startVirtualEsp32();
    try {
      const scan = new Uint8Array([0x47, 0x58, 0x52, 0x53, 0x43, 0x41, 0x4e, 0x31, 9, 8, 7]);
      await saveObjectScanArtifact('OBJ_0003', 'S000002', scan);
      const occupancy = buildSessionOccupancyArtifact(scanSnapshot(), scan, 1700000000000);
      await expect(saveSessionOccupancyArtifact('OBJ_0003', 'S000002', occupancy))
        .resolves.toMatchObject({ ok: true, bytes: occupancy.byteLength });

      const upload = esp.requests.find((request) => request.path.includes('kind=occupancy'));
      expect(upload).toMatchObject({
        method: 'POST',
        path: '/api/control?scan=save&object_id=OBJ_0003&session_id=S000002&kind=occupancy',
        contentType: 'application/octet-stream',
        bodyLength: occupancy.byteLength,
      });

      // Two artifacts, the scan intact.
      expect(esp.files.size).toBe(2);
      await expect(loadObjectScanArtifact('OBJ_0003', 'S000002')).resolves.toEqual(scan);
      await expect(fetchScanInfo('OBJ_0003', 'S000002', 'occupancy')).resolves.toEqual({ present: true, bytes: occupancy.byteLength });
      await expect(fetchScanInfo('OBJ_0003', 'S000002', 'scan')).resolves.toEqual({ present: true, bytes: scan.byteLength });

      // What the ESP32 holds decodes to exactly the scan's sectors.
      const stored = esp.files.get('OBJ_0003/sessions/S000002/occupancy-v1.bin');
      expect(stored).toBeDefined();
      const decoded = decodeOccupancySolverArtifact(stored as Uint8Array);
      expect(Array.from(decoded.free)).toEqual([0, 0, 1, 1, 0, 1]);
      expect(Array.from(decoded.occupied)).toEqual([0, 0, -1]);
      expect(decoded.header.below_surface_m).toBe(1.5);
      expect(decoded.header.frame_id).toBe(occupancyFrameIdForScan(scan));
      expect(decoded.header.frame_id).toBe(`scan-crc32:${occupancyCrc32(scan).toString(16).padStart(8, '0')}`);
      // No sensor<->scan registration yet: the ESP32 must fail closed.
      expect(decoded.header.registration).toBeNull();
    }
    finally {
      await esp.close();
    }
  });

  it('carries a registered void scene byte-exactly for the ESP32 methods', async () => {
    const esp = await startVirtualEsp32();
    try {
      const bytes = encodeVoidScene();
      await saveSessionOccupancyArtifact('OBJ_0001', 'S000001', bytes);
      const stored = esp.files.get('OBJ_0001/sessions/S000001/occupancy-v1.bin');
      expect(Array.from(stored ?? [])).toEqual(Array.from(bytes));
      const decoded = decodeOccupancySolverArtifact(stored as Uint8Array);
      expect(decoded.header.registration).toEqual(VOID_SCENE_REGISTRATION);
      expect(decoded.header.free_count).toBeGreaterThan(decoded.header.occupied_count);
    }
    finally {
      await esp.close();
    }
  });

  it('saves, loads, and reports the preview on the bulk scan lane', async () => {
    const esp = await startVirtualEsp32();
    try {
      const bytes = new Uint8Array([0x47, 0x58, 0x50, 0x52, 0x56, 0x31, 0, 0, 1, 0]);
      await expect(saveSessionPreviewArtifact('OBJ_0003', 'S000010', bytes)).resolves.toMatchObject({ ok: true, bytes: bytes.length });
      await expect(loadSessionPreviewArtifact('OBJ_0003', 'S000010')).resolves.toEqual(bytes);
      await expect(fetchScanInfo('OBJ_0003', 'S000010', 'preview')).resolves.toEqual({ present: true, bytes: bytes.length });
      const request = esp.requests[0];
      expect(request).toMatchObject({
        path: '/api/control?scan=save&object_id=OBJ_0003&session_id=S000010&kind=preview',
        contentType: 'application/octet-stream',
        bodyLength: bytes.length,
      });
    }
    finally {
      await esp.close();
    }
  });

  it('deleting the scan removes its derived occupancy and preview', async () => {
    const esp = await startVirtualEsp32();
    try {
      const scan = new Uint8Array([1, 2, 3]);
      await saveObjectScanArtifact('OBJ_0003', 'S000004', scan);
      await saveSessionOccupancyArtifact('OBJ_0003', 'S000004', buildSessionOccupancyArtifact(scanSnapshot(), scan, 1));
      await saveSessionPreviewArtifact('OBJ_0003', 'S000004', new Uint8Array([1, 2, 3]));
      await deleteObjectScanArtifact('OBJ_0003', 'S000004');
      await expect(fetchScanInfo('OBJ_0003', 'S000004', 'occupancy')).resolves.toEqual({ present: false, bytes: 0 });
      await expect(fetchScanInfo('OBJ_0003', 'S000004', 'scan')).resolves.toEqual({ present: false, bytes: 0 });
      await expect(loadObjectScanArtifact('OBJ_0003', 'S000004')).resolves.toBeNull();
      await expect(loadSessionPreviewArtifact('OBJ_0003', 'S000004')).resolves.toBeNull();
    }
    finally {
      await esp.close();
    }
  });

  it('keeps sessions apart and surfaces a device-side size refusal', async () => {
    const esp = await startVirtualEsp32();
    try {
      const scan = new Uint8Array([5, 5, 5]);
      await saveSessionOccupancyArtifact('OBJ_0003', 'S000005', buildSessionOccupancyArtifact(scanSnapshot(), scan, 1));
      await expect(fetchScanInfo('OBJ_0003', 'S000006', 'occupancy')).resolves.toEqual({ present: false, bytes: 0 });

      const oversized = new Uint8Array(OCCUPANCY_MAX_BYTES + 1);
      await expect(saveSessionOccupancyArtifact('OBJ_0003', 'S000005', oversized))
        .rejects.toThrow(/ESP scan save 400: .*ESP_ERR_INVALID_SIZE/);
      // The refused upload never replaced the stored occupancy.
      await expect(fetchScanInfo('OBJ_0003', 'S000005', 'occupancy')).resolves.toMatchObject({ present: true });
    }
    finally {
      await esp.close();
    }
  });

  it('the encoder refuses an artifact the ESP32 could not store', () => {
    expect(MAX_OCCUPANCY_ARTIFACT_BYTES).toBe(OCCUPANCY_MAX_BYTES);
    // 56^3 = 175,616 FREE cells x 6 bytes > 1 MiB, all within int16 indices.
    const side = 56;
    const free = new Float32Array(side * side * side * 3);
    let write = 0;
    for (let ix = 0; ix < side; ix += 1) {
      for (let iy = 0; iy < side; iy += 1) {
        for (let iz = 0; iz < side; iz += 1) {
          free[write++] = (ix + 0.5) * 0.18; free[write++] = (iy + 0.5) * 0.18; free[write++] = (iz + 0.5) * 0.18;
        }
      }
    }
    expect(() => buildSessionOccupancyArtifact({ ...scanSnapshot(), freeAirPositions: free }, new Uint8Array([1]), 1))
      .toThrow(/too large/);
  });
});
