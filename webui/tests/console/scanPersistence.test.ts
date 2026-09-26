import { afterEach, describe, expect, it } from 'vitest';

import type { ControlResult, ScanArtifactInfo } from '../../src/api/deviceApi';
import {
  ScanPersistenceController,
  transitionScanPersistence,
  type ScanPersistencePorts,
  type ScanPersistenceState,
} from '../../src/xr/scanPersistence';

function serverState(scan: 'saving' | 'saved' | 'error' = 'saved', occupancy: ScanPersistenceState['occupancy'] = 'pending'): ScanPersistenceState {
  return { scan, occupancy, scanError: null, occupancyError: null };
}

describe('scan persistence transitions', () => {
  it('covers each allowed save, retry, and reconciliation transition', () => {
    const saving = transitionScanPersistence(null, { type: 'scan-saving' });
    expect(saving).toEqual(serverState('saving'));
    expect(transitionScanPersistence(saving, { type: 'scan-saved' })).toEqual(serverState());
    expect(transitionScanPersistence(saving, { type: 'scan-failed', error: 'scan failed' })).toEqual({
      scan: 'error', occupancy: 'unavailable', scanError: 'scan failed', occupancyError: null,
    });

    const occupancySaving = transitionScanPersistence(serverState(), { type: 'occupancy-saving' });
    expect(occupancySaving).toEqual(serverState('saved', 'saving'));
    expect(transitionScanPersistence(occupancySaving, { type: 'occupancy-ready' })).toEqual(serverState('saved', 'ready'));
    expect(transitionScanPersistence(occupancySaving, { type: 'occupancy-failed', error: 'upload failed' })).toEqual({
      ...serverState('saved', 'failed'), occupancyError: 'upload failed',
    });
    expect(transitionScanPersistence(serverState(), { type: 'occupancy-unavailable', error: 'cannot encode' })).toEqual({
      ...serverState('saved', 'unavailable'), occupancyError: 'cannot encode',
    });
    expect(transitionScanPersistence(null, { type: 'reconcile-ready' })).toEqual(serverState('saved', 'ready'));
    expect(transitionScanPersistence(null, { type: 'reconcile-missing-retryable' })).toMatchObject({ scan: 'saved', occupancy: 'failed' });
    expect(transitionScanPersistence(null, { type: 'reconcile-missing-unavailable', error: 'rescan' })).toEqual({
      ...serverState('saved', 'unavailable'), occupancyError: 'rescan',
    });
    expect(transitionScanPersistence(null, { type: 'reconcile-error', error: 'network' })).toEqual({
      ...serverState('saved', 'unavailable'), occupancyError: 'network',
    });
  });

  it('rejects invalid transition requests', () => {
    const saving = transitionScanPersistence(null, { type: 'scan-saving' });
    const saved = transitionScanPersistence(saving, { type: 'scan-saved' });
    const scanError = transitionScanPersistence(saving, { type: 'scan-failed', error: 'failed' });
    expect(() => transitionScanPersistence(null, { type: 'scan-saved' })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(saving, { type: 'occupancy-saving' })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(saved, { type: 'occupancy-ready' })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(saved, { type: 'scan-failed', error: 'late failure' })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(scanError, { type: 'occupancy-saving' })).toThrow(/Invalid scan persistence transition/);
  });
});

interface TestServer {
  baseUrl: string;
  files: Map<string, Uint8Array>;
  scanUploadCount: number;
  occupancyUploads: Uint8Array[];
  acceptOccupancy: boolean;
  dropOccupancyConnection: boolean;
  close(): Promise<void>;
  ports: ScanPersistencePorts;
}

async function readBody(request: AsyncIterable<Uint8Array>): Promise<Uint8Array> {
  const chunks: Uint8Array[] = [];
  for await (const chunk of request) chunks.push(new Uint8Array(chunk));
  const body = new Uint8Array(chunks.reduce((sum, chunk) => sum + chunk.byteLength, 0));
  let offset = 0;
  for (const chunk of chunks) { body.set(chunk, offset); offset += chunk.byteLength; }
  return body;
}

async function startControlServer(): Promise<TestServer> {
  const { createServer, request: httpRequest } = await import('node:http');
  const files = new Map<string, Uint8Array>();
  const serverState = {
    scanUploadCount: 0,
    occupancyUploads: [] as Uint8Array[],
    acceptOccupancy: false,
    dropOccupancyConnection: false,
  };
  const server = createServer((request, response) => {
    void (async () => {
      const url = new URL(request.url ?? '/', `http://${request.headers.host ?? '127.0.0.1'}`);
      const body = await readBody(request);
      const action = url.searchParams.get('scan');
      const objectId = url.searchParams.get('object_id') ?? '';
      const sessionId = url.searchParams.get('session_id') ?? '';
      const kind = url.searchParams.get('kind') ?? 'scan';
      const key = `${objectId}/${sessionId}/${kind}`;
      const json = (status: number, result: unknown): void => {
        const text = JSON.stringify(result);
        response.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
        response.end(text);
      };

      if (request.method !== 'POST' || url.pathname !== '/api/control' || action === null) {
        json(404, { ok: false, error: 'route not found' });
        return;
      }
      if (action === 'save' && kind === 'scan') {
        serverState.scanUploadCount += 1;
        files.set(key, body);
        json(200, { ok: true, action: 'scan_save', bytes: body.byteLength });
        return;
      }
      if (action === 'save' && kind === 'occupancy') {
        serverState.occupancyUploads.push(body.slice());
        if (serverState.dropOccupancyConnection) {
          serverState.dropOccupancyConnection = false;
          response.destroy();
          return;
        }
        if (!serverState.acceptOccupancy) {
          json(500, { ok: false, action: 'scan_save', error: 'occupancy write failed' });
          return;
        }
        files.set(key, body);
        json(200, { ok: true, action: 'scan_save', bytes: body.byteLength });
        return;
      }
      if (action === 'info') {
        const saved = files.get(key);
        if (saved === undefined) json(404, { ok: false, error: 'ESP_ERR_NOT_FOUND' });
        else json(200, { ok: true, bytes: saved.byteLength });
        return;
      }
      json(400, { ok: false, error: 'unsupported request' });
    })().catch(() => response.destroy());
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (address === null || typeof address === 'string') throw new Error('HTTP test server did not bind an ephemeral TCP port.');
  const baseUrl = `http://127.0.0.1:${address.port}`;

  const post = (url: URL, bytes = new Uint8Array()): Promise<{ status: number; text: string }> => new Promise((resolve, reject) => {
    const outgoing = httpRequest(url, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' } }, (incoming) => {
      const chunks: Uint8Array[] = [];
      incoming.on('data', (chunk: Uint8Array) => chunks.push(new Uint8Array(chunk)));
      incoming.on('end', () => resolve({
        status: incoming.statusCode ?? 0,
        text: Buffer.concat(chunks.map((chunk) => Buffer.from(chunk))).toString('utf8'),
      }));
    });
    outgoing.on('error', reject);
    outgoing.end(Buffer.from(bytes));
  });
  const controlUrl = (objectId: string, sessionId: string, action: 'save' | 'info', kind: 'scan' | 'occupancy') => {
    const url = new URL('/api/control', baseUrl);
    url.searchParams.set('scan', action);
    url.searchParams.set('object_id', objectId);
    url.searchParams.set('session_id', sessionId);
    if (kind === 'occupancy') url.searchParams.set('kind', kind);
    return url;
  };
  const upload = async (objectId: string, sessionId: string, bytes: Uint8Array, kind: 'scan' | 'occupancy'): Promise<ControlResult> => {
    const response = await post(controlUrl(objectId, sessionId, 'save', kind), bytes);
    const value = JSON.parse(response.text) as ControlResult;
    if (response.status < 200 || response.status >= 300) throw new Error(`HTTP ${response.status}: ${value.error ?? 'save failed'}`);
    return value;
  };

  return {
    baseUrl,
    files,
    get scanUploadCount() { return serverState.scanUploadCount; },
    get occupancyUploads() { return serverState.occupancyUploads; },
    get acceptOccupancy() { return serverState.acceptOccupancy; },
    set acceptOccupancy(value: boolean) { serverState.acceptOccupancy = value; },
    get dropOccupancyConnection() { return serverState.dropOccupancyConnection; },
    set dropOccupancyConnection(value: boolean) { serverState.dropOccupancyConnection = value; },
    close: () => new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
    ports: {
      saveScan: (objectId, sessionId, bytes) => upload(objectId, sessionId, bytes, 'scan'),
      saveOccupancy: (objectId, sessionId, bytes) => upload(objectId, sessionId, bytes, 'occupancy'),
      fetchInfo: async (objectId, sessionId, kind): Promise<ScanArtifactInfo> => {
        const response = await post(controlUrl(objectId, sessionId, 'info', kind));
        if (response.status === 404) return { present: false, bytes: 0 };
        if (response.status < 200 || response.status >= 300) throw new Error(`HTTP ${response.status}: scan info failed`);
        const value = JSON.parse(response.text) as { bytes?: number };
        return { present: true, bytes: value.bytes ?? 0 };
      },
    },
  };
}

const scanBytes = new Uint8Array([0x47, 0x58, 0x52, 0x31, 3, 2, 1]);
const occupancyBytes = new Uint8Array([0x47, 0x4f, 0x43, 0x43, 5, 4, 3, 2]);

describe('scan persistence over real HTTP sockets', () => {
  let server: TestServer | null = null;
  afterEach(async () => {
    if (server !== null) await server.close();
    server = null;
  });

  it('saves the scan once, reports occupancy failure, and retries the exact same bytes', async () => {
    server = await startControlServer();
    const controller = new ScanPersistenceController(server.ports);

    const failed = await controller.save('OBJ_0001', 'S000001', scanBytes, () => occupancyBytes);
    expect(failed).toMatchObject({ scan: 'saved', occupancy: 'failed' });
    expect(server.files.get('OBJ_0001/S000001/scan')).toEqual(scanBytes);
    expect(server.files.has('OBJ_0001/S000001/occupancy')).toBe(false);

    server.acceptOccupancy = true;
    const retried = await controller.retry('OBJ_0001', 'S000001');
    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready' });
    expect(server.scanUploadCount).toBe(1);
    expect(server.occupancyUploads).toHaveLength(2);
    expect(server.occupancyUploads[0]).toEqual(occupancyBytes);
    expect(server.occupancyUploads[1]).toEqual(occupancyBytes);
  });

  it('keeps the scan saved when the occupancy connection drops', async () => {
    server = await startControlServer();
    server.dropOccupancyConnection = true;
    const controller = new ScanPersistenceController(server.ports);

    const state = await controller.save('OBJ_0001', 'S000002', scanBytes, () => occupancyBytes);
    expect(state).toMatchObject({ scan: 'saved', occupancy: 'failed' });
    expect(server.files.get('OBJ_0001/S000002/scan')).toEqual(scanBytes);
    expect(server.files.has('OBJ_0001/S000002/occupancy')).toBe(false);
  });

  it('reconciles a missing occupancy as retryable only with matching bytes in memory', async () => {
    server = await startControlServer();
    await server.ports.saveScan('OBJ_0001', 'S000003', scanBytes);
    const noMemory = new ScanPersistenceController(server.ports);
    await expect(noMemory.reconcile('OBJ_0001', 'S000003', scanBytes)).resolves.toMatchObject({
      scan: 'saved', occupancy: 'unavailable', occupancyError: expect.stringContaining('Rescan and re-save'),
    });

    const withMemory = new ScanPersistenceController(server.ports);
    await withMemory.save('OBJ_0001', 'S000003', scanBytes, () => occupancyBytes);
    await expect(withMemory.reconcile('OBJ_0001', 'S000003', scanBytes)).resolves.toMatchObject({ scan: 'saved', occupancy: 'failed' });

    server.acceptOccupancy = true;
    await withMemory.retry('OBJ_0001', 'S000003');
    await expect(noMemory.reconcile('OBJ_0001', 'S000003', scanBytes)).resolves.toMatchObject({ scan: 'saved', occupancy: 'ready' });
  });
});
