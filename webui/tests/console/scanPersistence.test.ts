import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  loadObjectScanArtifact,
  saveObjectScanArtifact,
  saveSessionOccupancyArtifact,
  saveSessionPreviewArtifact,
  ScanWriteOutcomeError,
  sendControl,
} from '../../src/api/deviceApi';
import type { ControlResult, ScanArtifactInfo } from '../../src/api/deviceApi';
import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import { occupancyFrameIdForScan } from '../../src/xr/occupancySolverArtifact';
import { createScanIdentity, getScanIdentityMetrics, resetScanIdentityMetrics } from '../../src/xr/scanIdentity';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';
import {
  ScanPersistenceController,
  SCAN_STORAGE_BLOCKED_MESSAGE,
  transitionScanPersistence,
  scanPersistenceSessionId,
  type ScanPersistencePorts,
  type ScanPersistenceState,
} from '../../src/xr/scanPersistence';

function serverState(scan: ScanPersistenceState['scan'] = 'saved', occupancy: ScanPersistenceState['occupancy'] = 'pending', scanId = 'scan-crc32:00000000'): ScanPersistenceState {
  return {
    scan, occupancy, preview: 'pending', scanError: null, occupancyError: null, previewError: null,
    scanId, outcome: scan === 'saved' ? 'confirmed_committed' : null, previousKept: false,
  };
}

describe('scan persistence transitions', () => {
  it('covers each allowed save, retry, and reconciliation transition', () => {
    const scanId = 'scan-crc32:00000000';
    const saving = transitionScanPersistence(null, { type: 'scan-saving', scanId });
    expect(saving).toEqual(serverState('saving', 'pending', scanId));
    expect(transitionScanPersistence(saving, { type: 'scan-saved', scanId })).toEqual(serverState('saved', 'pending', scanId));
    expect(transitionScanPersistence(saving, { type: 'scan-not-committed', scanId, error: 'scan failed', previousKept: true })).toMatchObject({
      scan: 'error', scanError: 'scan failed', outcome: 'confirmed_not_committed', previousKept: true,
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
    const reconciled = transitionScanPersistence(null, { type: 'reconcile-saved', scanId });
    expect(transitionScanPersistence(reconciled, { type: 'reconcile-occupancy-ready' })).toEqual(serverState('saved', 'ready', scanId));
    expect(transitionScanPersistence(reconciled, { type: 'reconcile-occupancy-missing-retryable' })).toMatchObject({ scan: 'saved', occupancy: 'failed' });
    expect(transitionScanPersistence(reconciled, { type: 'reconcile-occupancy-unavailable', error: 'rescan' })).toEqual({
      ...serverState('saved', 'unavailable'), occupancyError: 'rescan',
    });
    expect(transitionScanPersistence(null, { type: 'reconcile-unknown', scanId: null, error: 'network' })).toMatchObject({
      scan: 'unknown', outcome: 'unknown', scanError: 'network',
    });
  });

  it('rejects invalid transition requests', () => {
    const scanId = 'scan-crc32:00000000';
    const saving = transitionScanPersistence(null, { type: 'scan-saving', scanId });
    const saved = transitionScanPersistence(saving, { type: 'scan-saved', scanId });
    const scanError = transitionScanPersistence(saving, { type: 'scan-not-committed', scanId, error: 'failed', previousKept: false });
    expect(() => transitionScanPersistence(null, { type: 'scan-saved', scanId })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(saving, { type: 'occupancy-saving' })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(saved, { type: 'occupancy-ready' })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(saved, { type: 'scan-saved', scanId })).toThrow(/Invalid scan persistence transition/);
    expect(() => transitionScanPersistence(scanError, { type: 'occupancy-saving' })).toThrow(/Invalid scan persistence transition/);
  });
});

interface TestServer {
  baseUrl: string;
  files: Map<string, Uint8Array>;
  scanSaveAttempts: number;
  scanUploadCount: number;
  occupancyUploads: Uint8Array[];
  previewUploads: Uint8Array[];
  derivativeScanIds: string[];
  acceptOccupancy: boolean;
  failPreview: boolean;
  dropOccupancyConnection: boolean;
  dropScanResponseAfterCommit: boolean;
  closeScanBeforeCommit: boolean;
  scanResponseDelayMs: number;
  scanTimeoutMs: number;
  failInfo: boolean;
  seed(objectId: string, sessionId: string, kind: 'scan' | 'occupancy' | 'preview', bytes: Uint8Array, sourceId?: string): void;
  recordingBusy: boolean;
  truncateScanLoad: boolean;
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
  const sourceIds = new Map<string, string>();
  const serverState = {
    scanSaveAttempts: 0,
    scanUploadCount: 0,
    occupancyUploads: [] as Uint8Array[],
    previewUploads: [] as Uint8Array[],
    derivativeScanIds: [] as string[],
    acceptOccupancy: false,
    failPreview: false,
    dropOccupancyConnection: false,
    dropScanResponseAfterCommit: false,
    closeScanBeforeCommit: false,
    scanResponseDelayMs: 0,
    scanTimeoutMs: 0,
    failInfo: false,
    recordingBusy: false,
    truncateScanLoad: false,
  };
  const server = createServer((request, response) => {
    void (async () => {
      const url = new URL(request.url ?? '/', `http://${request.headers.host ?? '127.0.0.1'}`);
      const action = url.searchParams.get('scan');
      const objectId = url.searchParams.get('object_id') ?? '';
      const sessionId = url.searchParams.get('session_id') ?? '';
      const kind = url.searchParams.get('kind') ?? 'scan';
      const key = `${objectId}/${sessionId}/${kind}`;
      const scanKey = `${objectId}/${sessionId}/scan`;
      const json = (status: number, result: unknown): void => {
        const text = JSON.stringify(result);
        response.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
        response.end(text);
      };

      if (request.method !== 'POST' || url.pathname !== '/api/control' || action === null) {
        json(404, { ok: false, error: 'route not found' });
        return;
      }
      if (action === 'save' && kind === 'scan' && serverState.closeScanBeforeCommit) {
        serverState.closeScanBeforeCommit = false;
        request.once('data', () => request.destroy());
        return;
      }
      const body = await readBody(request);
      if (action === 'save' && kind === 'scan') {
        serverState.scanSaveAttempts += 1;
        if (serverState.recordingBusy) {
          json(409, { ok: false, action: 'scan_save', busy_reason: 'recording_active' });
          return;
        }
        serverState.scanUploadCount += 1;
        for (const derivedKind of ['occupancy', 'preview']) {
          files.delete(`${objectId}/${sessionId}/${derivedKind}`);
          sourceIds.delete(`${objectId}/${sessionId}/${derivedKind}`);
        }
        files.set(key, body);
        const result = { ok: true, action: 'scan_save', bytes: body.byteLength, id: occupancyFrameIdForScan(body) };
        if (serverState.dropScanResponseAfterCommit) {
          serverState.dropScanResponseAfterCommit = false;
          response.destroy();
          return;
        }
        if (serverState.scanResponseDelayMs > 0) {
          globalThis.setTimeout(() => { if (!response.destroyed) json(200, result); }, serverState.scanResponseDelayMs);
          return;
        }
        json(200, result);
        return;
      }
      if (action === 'save' && kind === 'occupancy') {
        serverState.occupancyUploads.push(body.slice());
        if (serverState.dropOccupancyConnection) {
          serverState.dropOccupancyConnection = false;
          response.destroy();
          return;
        }
        const scanId = url.searchParams.get('scan_id');
        if (scanId !== null) serverState.derivativeScanIds.push(scanId);
        const currentScan = files.get(scanKey);
        const currentId = currentScan === undefined ? null : occupancyFrameIdForScan(currentScan);
        if (scanId !== null && scanId !== currentId) {
          json(409, { ok: false, action: 'scan_save', error: 'scan_generation_mismatch', current_id: currentId });
          return;
        }
        if (!serverState.acceptOccupancy) {
          json(500, { ok: false, action: 'scan_save', error: 'occupancy write failed' });
          return;
        }
        files.set(key, body);
        if (scanId !== null) sourceIds.set(key, scanId);
        json(200, { ok: true, action: 'scan_save', bytes: body.byteLength });
        return;
      }
      if (action === 'save' && kind === 'preview') {
        serverState.previewUploads.push(body.slice());
        const scanId = url.searchParams.get('scan_id');
        if (scanId !== null) serverState.derivativeScanIds.push(scanId);
        const currentScan = files.get(scanKey);
        const currentId = currentScan === undefined ? null : occupancyFrameIdForScan(currentScan);
        if (scanId !== null && scanId !== currentId) {
          json(409, { ok: false, action: 'scan_save', error: 'scan_generation_mismatch', current_id: currentId });
          return;
        }
        if (serverState.failPreview) {
          json(500, { ok: false, action: 'scan_save', error: 'preview write failed' });
          return;
        }
        files.set(key, body);
        if (scanId !== null) sourceIds.set(key, scanId);
        json(200, { ok: true, action: 'scan_save', bytes: body.byteLength });
        return;
      }
      if (action === 'info') {
        if (serverState.failInfo) {
          json(500, { ok: false, error: 'scan info unavailable' });
          return;
        }
        const saved = files.get(key);
        if (saved === undefined) json(404, { ok: false, error: 'ESP_ERR_NOT_FOUND' });
        else json(200, kind === 'scan'
          ? { ok: true, bytes: saved.byteLength, id: occupancyFrameIdForScan(saved) }
          : { ok: true, bytes: saved.byteLength, source_id: sourceIds.get(key) ?? null });
        return;
      }
      if (action === 'load' && kind === 'scan') {
        const saved = files.get(key);
        if (saved === undefined) json(404, { ok: false, error: 'ESP_ERR_NOT_FOUND' });
        else {
          const bytes = serverState.truncateScanLoad ? saved.slice(0, -1) : saved;
          response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': bytes.byteLength });
          response.end(Buffer.from(bytes));
        }
        return;
      }
      json(400, { ok: false, error: 'unsupported request' });
    })().catch(() => response.destroy());
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (address === null || typeof address === 'string') throw new Error('HTTP test server did not bind an ephemeral TCP port.');
  const baseUrl = `http://127.0.0.1:${address.port}`;

  const post = (url: URL, bytes = new Uint8Array(), timeoutMs = 0): Promise<{ status: number; text: string }> => new Promise((resolve, reject) => {
    const outgoing = httpRequest(url, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' } }, (incoming) => {
      const chunks: Uint8Array[] = [];
      incoming.on('data', (chunk: Uint8Array) => chunks.push(new Uint8Array(chunk)));
      incoming.on('end', () => resolve({
        status: incoming.statusCode ?? 0,
        text: Buffer.concat(chunks.map((chunk) => Buffer.from(chunk))).toString('utf8'),
      }));
    });
    outgoing.on('error', reject);
    if (timeoutMs > 0) outgoing.setTimeout(timeoutMs, () => outgoing.destroy(new Error('request timeout')));
    outgoing.end(Buffer.from(bytes));
  });
  const controlUrl = (objectId: string, sessionId: string, action: 'save' | 'info', kind: 'scan' | 'occupancy' | 'preview', scanId?: string) => {
    const url = new URL('/api/control', baseUrl);
    url.searchParams.set('scan', action);
    url.searchParams.set('object_id', objectId);
    url.searchParams.set('session_id', sessionId);
    if (kind !== 'scan') url.searchParams.set('kind', kind);
    if (scanId !== undefined) url.searchParams.set('scan_id', scanId);
    return url;
  };
  const upload = async (objectId: string, sessionId: string, bytes: Uint8Array, kind: 'scan' | 'occupancy' | 'preview', scanId?: string): Promise<ControlResult> => {
    const response = await post(controlUrl(objectId, sessionId, 'save', kind, scanId), bytes, kind === 'scan' ? serverState.scanTimeoutMs : 0);
    const value = JSON.parse(response.text) as ControlResult;
    if (response.status === 409 && value.busy_reason === 'recording_active') {
      return { ...value, outcome: 'blocked_recording' };
    }
    if (response.status === 409 && value.error === 'scan_generation_mismatch') {
      throw new ScanWriteOutcomeError('confirmed_not_committed', value.error, response.status, value.current_id ?? null);
    }
    if (response.status < 200 || response.status >= 300) {
      throw new ScanWriteOutcomeError('confirmed_not_committed', `HTTP ${response.status}: ${value.error ?? 'save failed'}`, response.status);
    }
    return value;
  };

  return {
    baseUrl,
    files,
    get scanSaveAttempts() { return serverState.scanSaveAttempts; },
    get scanUploadCount() { return serverState.scanUploadCount; },
    get occupancyUploads() { return serverState.occupancyUploads; },
    get previewUploads() { return serverState.previewUploads; },
    get derivativeScanIds() { return serverState.derivativeScanIds; },
    seed(objectId: string, sessionId: string, kind: 'scan' | 'occupancy' | 'preview', bytes: Uint8Array, sourceId?: string) {
      const key = `${objectId}/${sessionId}/${kind}`;
      files.set(key, bytes.slice());
      if (sourceId !== undefined) sourceIds.set(key, sourceId);
      else sourceIds.delete(key);
    },
    get acceptOccupancy() { return serverState.acceptOccupancy; },
    set acceptOccupancy(value: boolean) { serverState.acceptOccupancy = value; },
    get failPreview() { return serverState.failPreview; },
    set failPreview(value: boolean) { serverState.failPreview = value; },
    get dropOccupancyConnection() { return serverState.dropOccupancyConnection; },
    set dropOccupancyConnection(value: boolean) { serverState.dropOccupancyConnection = value; },
    get dropScanResponseAfterCommit() { return serverState.dropScanResponseAfterCommit; },
    set dropScanResponseAfterCommit(value: boolean) { serverState.dropScanResponseAfterCommit = value; },
    get closeScanBeforeCommit() { return serverState.closeScanBeforeCommit; },
    set closeScanBeforeCommit(value: boolean) { serverState.closeScanBeforeCommit = value; },
    get scanResponseDelayMs() { return serverState.scanResponseDelayMs; },
    set scanResponseDelayMs(value: number) { serverState.scanResponseDelayMs = value; },
    get scanTimeoutMs() { return serverState.scanTimeoutMs; },
    set scanTimeoutMs(value: number) { serverState.scanTimeoutMs = value; },
    get failInfo() { return serverState.failInfo; },
    set failInfo(value: boolean) { serverState.failInfo = value; },
    get recordingBusy() { return serverState.recordingBusy; },
    set recordingBusy(value: boolean) { serverState.recordingBusy = value; },
    get truncateScanLoad() { return serverState.truncateScanLoad; },
    set truncateScanLoad(value: boolean) { serverState.truncateScanLoad = value; },
    close: () => new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
    ports: {
      saveScan: (objectId, sessionId, bytes) => upload(objectId, sessionId, bytes, 'scan'),
      saveOccupancy: (objectId, sessionId, bytes, scanId) => upload(objectId, sessionId, bytes, 'occupancy', scanId),
      savePreview: (objectId, sessionId, bytes, scanId) => upload(objectId, sessionId, bytes, 'preview', scanId),
      fetchInfo: async (objectId, sessionId, kind): Promise<ScanArtifactInfo> => {
        const response = await post(controlUrl(objectId, sessionId, 'info', kind));
        if (response.status === 404) return { present: false, bytes: 0, id: null, sourceId: null };
        if (response.status < 200 || response.status >= 300) throw new Error(`HTTP ${response.status}: scan info failed`);
        const value = JSON.parse(response.text) as { bytes?: number; id?: string; source_id?: string };
        return {
          present: true,
          bytes: value.bytes ?? 0,
          id: value.id ?? null,
          sourceId: value.source_id ?? null,
        };
      },
    },
  };
}

const scanBytes = new Uint8Array([0x47, 0x58, 0x52, 0x31, 3, 2, 1]);
const occupancyBytes = new Uint8Array([0x47, 0x4f, 0x43, 0x43, 5, 4, 3, 2]);
const previewBytes = new Uint8Array([0x47, 0x58, 0x50, 0x52, 0x56, 0x31, 1, 0]);

describe('scan persistence over real HTTP sockets', () => {
  let server: TestServer | null = null;
  afterEach(async () => {
    vi.unstubAllGlobals();
    if (server !== null) await server.close();
    server = null;
  });

  it('saves the scan once, reports occupancy failure, and retries the exact same bytes', async () => {
    server = await startControlServer();
    const controller = new ScanPersistenceController(server.ports);

    const failed = await controller.save('OBJ_0001', 'S000001', scanBytes, () => occupancyBytes, () => previewBytes);
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

  it('verifies a committed scan after the reply is dropped and builds B derivatives once', async () => {
    server = await startControlServer();
    server.acceptOccupancy = true;
    server.dropScanResponseAfterCommit = true;
    const observed: ScanPersistenceState[] = [];
    const controller = new ScanPersistenceController(server.ports, (state) => { if (state !== null) observed.push(state); });

    const saved = await controller.save('OBJ_0001', 'S000011', scanBytes, () => occupancyBytes, () => previewBytes);
    const scanId = occupancyFrameIdForScan(scanBytes);

    expect(observed.map((state) => state.scan)).toContain('verifying');
    expect(saved).toMatchObject({ scan: 'saved', outcome: 'confirmed_committed', scanId, occupancy: 'ready', preview: 'ready' });
    expect(server.scanUploadCount).toBe(1);
    expect(server.occupancyUploads).toEqual([occupancyBytes]);
    expect(server.previewUploads).toEqual([previewBytes]);
    expect(server.derivativeScanIds).toEqual([scanId, scanId]);
  });

  it('proves the previous scan stayed after a pre-commit socket close and retains B for retry', async () => {
    server = await startControlServer();
    const objectId = 'OBJ_0001';
    const sessionId = 'S000012';
    const previousBytes = new Uint8Array([10, 11, 12]);
    await server.ports.saveScan(objectId, sessionId, previousBytes);
    server.closeScanBeforeCommit = true;
    const controller = new ScanPersistenceController(server.ports);

    const failed = await controller.save(objectId, sessionId, scanBytes, () => occupancyBytes, () => previewBytes);

    expect(failed).toMatchObject({ scan: 'error', outcome: 'confirmed_not_committed', previousKept: true });
    expect(server.files.get(`${objectId}/${sessionId}/scan`)).toEqual(previousBytes);
    expect(server.occupancyUploads).toHaveLength(0);
    expect(server.previewUploads).toHaveLength(0);
    expect(controller.retryAvailable).toBe(true);

    server.acceptOccupancy = true;
    const retried = await controller.retry(objectId, sessionId);
    expect(retried).toMatchObject({ scan: 'saved', scanId: occupancyFrameIdForScan(scanBytes) });
    expect(server.scanUploadCount).toBe(2);
    expect(server.files.get(`${objectId}/${sessionId}/scan`)).toEqual(scanBytes);
  });

  it('keeps the candidate unknown when response and verification are unavailable', async () => {
    server = await startControlServer();
    server.dropScanResponseAfterCommit = true;
    server.failInfo = true;
    const controller = new ScanPersistenceController(server.ports);

    const unknown = await controller.save('OBJ_0001', 'S000013', scanBytes, () => occupancyBytes, () => previewBytes);

    expect(unknown).toMatchObject({ scan: 'unknown', outcome: 'unknown', scanId: occupancyFrameIdForScan(scanBytes) });
    expect(controller.retryAvailable).toBe(true);
    expect(server.files.get('OBJ_0001/S000013/scan')).toEqual(scanBytes);

    server.failInfo = false;
    const verified = await controller.retry('OBJ_0001', 'S000013');
    expect(verified).toMatchObject({ scan: 'saved', outcome: 'confirmed_committed' });
    expect(server.scanUploadCount).toBe(1);
    expect(server.occupancyUploads[0]).toEqual(occupancyBytes);
  });

  it('verifies a scan committed before its delayed reply times out', async () => {
    server = await startControlServer();
    server.acceptOccupancy = true;
    server.scanTimeoutMs = 5;
    server.scanResponseDelayMs = 35;
    const observed: ScanPersistenceState[] = [];
    const controller = new ScanPersistenceController(server.ports, (state) => { if (state !== null) observed.push(state); });

    const saved = await controller.save('OBJ_0001', 'S000014', scanBytes, () => occupancyBytes, () => previewBytes);

    expect(observed.map((state) => state.scan)).toContain('verifying');
    expect(saved).toMatchObject({ scan: 'saved', scanId: occupancyFrameIdForScan(scanBytes), occupancy: 'ready', preview: 'ready' });
    expect(server.scanUploadCount).toBe(1);
    expect(server.derivativeScanIds).toEqual([occupancyFrameIdForScan(scanBytes), occupancyFrameIdForScan(scanBytes)]);
  });

  it('invalidates A derivatives at B commit and retries only the retained B occupancy bytes', async () => {
    server = await startControlServer();
    const objectId = 'OBJ_0001';
    const sessionId = 'S000015';
    const previousBytes = new Uint8Array([21, 22, 23]);
    const previousId = occupancyFrameIdForScan(previousBytes);
    server.seed(objectId, sessionId, 'scan', previousBytes);
    server.seed(objectId, sessionId, 'occupancy', new Uint8Array([1, 2]), previousId);
    server.seed(objectId, sessionId, 'preview', new Uint8Array([3, 4]), previousId);
    server.occupancyUploads.splice(0);
    server.previewUploads.splice(0);
    const controller = new ScanPersistenceController(server.ports);

    const failed = await controller.save(objectId, sessionId, scanBytes, () => occupancyBytes, () => previewBytes);

    expect(failed).toMatchObject({ scan: 'saved', occupancy: 'failed', preview: 'ready' });
    expect(server.files.get(`${objectId}/${sessionId}/scan`)).toEqual(scanBytes);
    expect(server.files.has(`${objectId}/${sessionId}/occupancy`)).toBe(false);
    expect(server.files.get(`${objectId}/${sessionId}/preview`)).toEqual(previewBytes);
    expect(server.occupancyUploads).toEqual([occupancyBytes]);

    server.acceptOccupancy = true;
    const retried = await controller.retry(objectId, sessionId);
    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready' });
    expect(server.occupancyUploads).toEqual([occupancyBytes, occupancyBytes]);
    expect(server.derivativeScanIds).toEqual([occupancyFrameIdForScan(scanBytes), occupancyFrameIdForScan(scanBytes), occupancyFrameIdForScan(scanBytes)]);
  });

  it('marks old and null-source derivatives stale and rebuilds them for the loaded scan ID', async () => {
    server = await startControlServer();
    const objectId = 'OBJ_0001';
    const scanId = occupancyFrameIdForScan(scanBytes);
    const oldId = occupancyFrameIdForScan(new Uint8Array([90]));
    server.seed(objectId, 'S000016', 'scan', scanBytes);
    server.seed(objectId, 'S000016', 'occupancy', new Uint8Array([90]), oldId);
    const stale = await new ScanPersistenceController(server.ports).reconcile(
      objectId, 'S000016', scanBytes, () => occupancyBytes, () => previewBytes,
    );
    expect(stale).toMatchObject({ scan: 'saved', scanId, occupancy: 'stale', preview: 'failed' });
    server.acceptOccupancy = true;
    const staleController = new ScanPersistenceController(server.ports);
    await staleController.reconcile(objectId, 'S000016', scanBytes, () => occupancyBytes, () => previewBytes);
    const rebuilt = await staleController.retry(objectId, 'S000016');
    expect(rebuilt).toMatchObject({ occupancy: 'ready', preview: 'ready' });
    expect(server.occupancyUploads[server.occupancyUploads.length - 1]).toEqual(occupancyBytes);
    expect(server.derivativeScanIds.slice(-2)).toEqual([scanId, scanId]);

    server.seed(objectId, 'S000017', 'scan', scanBytes);
    server.seed(objectId, 'S000017', 'occupancy', occupancyBytes, scanId);
    server.seed(objectId, 'S000017', 'preview', previewBytes, scanId);
    const matching = await new ScanPersistenceController(server.ports).reconcile(objectId, 'S000017', scanBytes);
    expect(matching).toMatchObject({ occupancy: 'ready', preview: 'ready' });

    server.seed(objectId, 'S000018', 'scan', scanBytes);
    server.seed(objectId, 'S000018', 'occupancy', occupancyBytes);
    const nullSource = await new ScanPersistenceController(server.ports).reconcile(
      objectId, 'S000018', scanBytes, () => occupancyBytes, () => previewBytes,
    );
    expect(nullSource).toMatchObject({ occupancy: 'stale' });
  });

  it('stops on a derivative generation 409 and does not retry or upload preview', async () => {
    server = await startControlServer();
    const objectId = 'OBJ_0001';
    const sessionId = 'S000019';
    const originalSave = server.ports.saveScan;
    const ports: ScanPersistencePorts = {
      ...server.ports,
      saveScan: async (object, session, bytes) => {
        const result = await originalSave(object, session, bytes);
        await originalSave(object, session, new Uint8Array([70, 71, 72]));
        return result;
      },
    };
    const controller = new ScanPersistenceController(ports);

    const stale = await controller.save(objectId, sessionId, scanBytes, () => occupancyBytes, () => previewBytes);

    expect(stale).toMatchObject({ scan: 'derived_stale', occupancy: 'stale', outcome: 'unknown' });
    expect(server.occupancyUploads).toEqual([occupancyBytes]);
    expect(server.previewUploads).toHaveLength(0);
    expect(controller.retryAvailable).toBe(false);
  });

  it('saves a pre-recording scan under the reserved staging target', async () => {
    server = await startControlServer();
    server.acceptOccupancy = true;
    const controller = new ScanPersistenceController(server.ports);
    const sessionId = scanPersistenceSessionId(null);

    expect(sessionId).toBe('staging');
    const saved = await controller.save('OBJ_0001', sessionId, scanBytes, () => occupancyBytes, () => previewBytes);

    expect(saved).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
    expect(server.files.get('OBJ_0001/staging/scan')).toEqual(scanBytes);
    expect(server.files.get('OBJ_0001/staging/occupancy')).toEqual(occupancyBytes);
    expect(server.files.get('OBJ_0001/staging/preview')).toEqual(previewBytes);
  });

  it('keeps the scan saved when the occupancy connection drops', async () => {
    server = await startControlServer();
    server.dropOccupancyConnection = true;
    const controller = new ScanPersistenceController(server.ports);

    const state = await controller.save('OBJ_0001', 'S000002', scanBytes, () => occupancyBytes, () => previewBytes);
    expect(state).toMatchObject({ scan: 'saved', occupancy: 'failed' });
    expect(server.files.get('OBJ_0001/S000002/scan')).toEqual(scanBytes);
    expect(server.files.has('OBJ_0001/S000002/occupancy')).toBe(false);
  });

  it('preview failure leaves the saved scan and occupancy intact and retries only preview', async () => {
    server = await startControlServer();
    server.acceptOccupancy = true;
    server.failPreview = true;
    const controller = new ScanPersistenceController(server.ports);

    const failed = await controller.save('OBJ_0001', 'S000007', scanBytes, () => occupancyBytes, () => previewBytes);
    expect(failed).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'failed' });
    expect(server.files.get('OBJ_0001/S000007/scan')).toEqual(scanBytes);
    expect(server.files.get('OBJ_0001/S000007/occupancy')).toEqual(occupancyBytes);
    expect(server.files.has('OBJ_0001/S000007/preview')).toBe(false);

    server.failPreview = false;
    const retried = await controller.retry('OBJ_0001', 'S000007');
    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
    expect(server.scanUploadCount).toBe(1);
    expect(server.occupancyUploads).toHaveLength(1);
    expect(server.previewUploads).toHaveLength(2);
    expect(server.previewUploads[0]).toEqual(previewBytes);
    expect(server.previewUploads[1]).toEqual(previewBytes);
  });

  it('does not reuse a session A preview when retry is requested for session B', async () => {
    server = await startControlServer();
    server.acceptOccupancy = true;
    server.failPreview = true;
    const controller = new ScanPersistenceController(server.ports);
    await controller.save('OBJ_0001', 'S000008', scanBytes, () => occupancyBytes, () => previewBytes);
    await server.ports.saveScan('OBJ_0001', 'S000009', scanBytes);
    await controller.retry('OBJ_0001', 'S000009');
    expect(server.files.has('OBJ_0001/S000009/preview')).toBe(false);
    expect(server.previewUploads).toHaveLength(1);
  });

  it('keeps bytes after a recording 409 and retries one successful scan upload', async () => {
    server = await startControlServer();
    server.recordingBusy = true;
    const originalBytes = scanBytes.slice();
    const controller = new ScanPersistenceController(server.ports);

    const blocked = await controller.save('OBJ_0001', 'S000004', originalBytes, () => occupancyBytes, () => previewBytes);
    expect(blocked).toMatchObject({ scan: 'blocked_recording', scanError: SCAN_STORAGE_BLOCKED_MESSAGE });
    expect(controller.retryAvailable).toBe(true);
    expect(server.scanSaveAttempts).toBe(1);
    expect(server.scanUploadCount).toBe(0);
    originalBytes.fill(0);

    server.recordingBusy = false;
    server.acceptOccupancy = true;
    const retried = await controller.retry('OBJ_0001', 'S000004');

    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready' });
    expect(server.scanSaveAttempts).toBe(2);
    expect(server.scanUploadCount).toBe(1);
    expect(server.files.get('OBJ_0001/S000004/scan')).toEqual(scanBytes);
  });

  it('rejects a truncated scan loaded from a real HTTP response with its CRC', async () => {
    server = await startControlServer();
    const artifact = encodeScanArtifact(makeValidScanSnapshot());
    server.files.set('OBJ_0001/S000005/scan', artifact);
    server.truncateScanLoad = true;
    const nativeFetch = globalThis.fetch;
    vi.stubGlobal('fetch', (input: RequestInfo | URL, init?: RequestInit) =>
      nativeFetch(new URL(String(input), server!.baseUrl), init));

    const downloaded = await loadObjectScanArtifact('OBJ_0001', 'S000005');

    expect(downloaded).not.toBeNull();
    expect(() => decodeScanArtifact(downloaded!)).toThrow(/checksum/i);
  });

  it('reconciles a missing occupancy as retryable only with matching bytes in memory', async () => {
    server = await startControlServer();
    await server.ports.saveScan('OBJ_0001', 'S000003', scanBytes);
    const noMemory = new ScanPersistenceController(server.ports);
    await expect(noMemory.reconcile('OBJ_0001', 'S000003', scanBytes)).resolves.toMatchObject({
      scan: 'saved', occupancy: 'unavailable', occupancyError: expect.stringContaining('Rescan and re-save'),
    });

    const withMemory = new ScanPersistenceController(server.ports);
    await withMemory.save('OBJ_0001', 'S000003', scanBytes, () => occupancyBytes, () => previewBytes);
    await expect(withMemory.reconcile('OBJ_0001', 'S000003', scanBytes)).resolves.toMatchObject({ scan: 'saved', occupancy: 'failed' });

    server.acceptOccupancy = true;
    await withMemory.retry('OBJ_0001', 'S000003');
    await expect(noMemory.reconcile('OBJ_0001', 'S000003', scanBytes)).resolves.toMatchObject({ scan: 'saved', occupancy: 'ready' });
  });
});

describe('scan persistence after bulk preemption', () => {
  afterEach(() => vi.unstubAllGlobals());

  it('keeps scan bytes retryable when a critical control preempts the upload', async () => {
    const uploads: Uint8Array[] = [];
    let firstSignal!: AbortSignal;
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      if (url.includes('scan=save')) {
        uploads.push(new Uint8Array(init?.body as ArrayBuffer));
        if (uploads.length === 1) {
          firstSignal = init?.signal as AbortSignal;
          return new Promise<Response>((_resolve, reject) => {
            firstSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
          });
        }
        return Promise.resolve(new Response(JSON.stringify({
          ok: true, action: 'scan_save', id: occupancyFrameIdForScan(scanBytes),
        }), { status: 200 }));
      }
      return Promise.resolve(new Response(JSON.stringify({ ok: true, action: 'measurement_stop' }), { status: 200 }));
    });
    vi.stubGlobal('fetch', fetchMock);
    const ports: ScanPersistencePorts = {
      saveScan: saveObjectScanArtifact,
      saveOccupancy: async () => ({ ok: true, action: 'scan_save' }),
      savePreview: async () => ({ ok: true, action: 'scan_save' }),
      fetchInfo: async () => ({ present: false, bytes: 0, id: null, sourceId: null }),
    };
    const controller = new ScanPersistenceController(ports);

    const saving = controller.save('OBJ_0001', 'S000006', scanBytes, () => occupancyBytes, () => previewBytes);
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    const stopping = sendControl('measurement_stop');
    expect(firstSignal.aborted).toBe(true);
    const [paused] = await Promise.all([saving, stopping]);

    expect(paused).toMatchObject({ scan: 'error', outcome: 'confirmed_not_committed', previousKept: false });
    expect(controller.retryAvailable).toBe(true);
    expect(uploads[0]).toEqual(scanBytes);

    const retried = await controller.retry('OBJ_0001', 'S000006');
    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready' });
    expect(uploads).toEqual([scanBytes, scanBytes]);
  });

  it('leaves occupancy pending when STOP preempts its upload, then retries the same bytes', async () => {
    const candidateId = occupancyFrameIdForScan(scanBytes);
    let occupancyAttempts = 0;
    let previewAttempts = 0;
    let occupancySignal!: AbortSignal;
    let signalOccupancyStarted!: () => void;
    const occupancyStarted = new Promise<void>((resolve) => { signalOccupancyStarted = resolve; });
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      if (url.includes('measurement_stop')) {
        return Promise.resolve(new Response(JSON.stringify({ ok: true, action: 'measurement_stop' }), { status: 200 }));
      }
      if (url.includes('kind=occupancy')) {
        occupancyAttempts += 1;
        if (occupancyAttempts === 1) {
          occupancySignal = init?.signal as AbortSignal;
          signalOccupancyStarted();
          return new Promise<Response>((_resolve, reject) => {
            occupancySignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
          });
        }
        return Promise.resolve(new Response(JSON.stringify({ ok: true, action: 'scan_save' }), { status: 200 }));
      }
      if (url.includes('kind=preview')) {
        previewAttempts += 1;
        return Promise.resolve(new Response(JSON.stringify({ ok: true, action: 'scan_save' }), { status: 200 }));
      }
      return Promise.resolve(new Response(JSON.stringify({ ok: true }), { status: 200 }));
    });
    vi.stubGlobal('fetch', fetchMock);
    const ports: ScanPersistencePorts = {
      saveScan: async () => ({ ok: true, action: 'scan_save', id: candidateId }),
      saveOccupancy: saveSessionOccupancyArtifact,
      savePreview: saveSessionPreviewArtifact,
      fetchInfo: async () => ({ present: false, bytes: 0, id: null, sourceId: null }),
    };
    const controller = new ScanPersistenceController(ports);
    const saving = controller.save('OBJ_0001', 'S000020', scanBytes, () => occupancyBytes, () => previewBytes);
    await occupancyStarted;
    const stopping = sendControl('measurement_stop');
    expect(occupancySignal.aborted).toBe(true);
    const [paused, stopResult] = await Promise.all([saving, stopping]);

    expect(stopResult.ok).toBe(true);
    expect(paused).toMatchObject({ scan: 'saved', occupancy: 'failed', preview: 'pending' });
    expect(previewAttempts).toBe(0);
    expect(fetchMock.mock.calls[0][0]).toContain(`scan_id=${encodeURIComponent(candidateId)}`);

    const retried = await controller.retry('OBJ_0001', 'S000020');
    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
    expect(occupancyAttempts).toBe(2);
    expect(previewAttempts).toBe(1);
  });
});

describe('asynchronous derivative builders (MAX-403)', () => {
  function fakePorts() {
    const calls: string[] = [];
    const ports: ScanPersistencePorts = {
      saveScan: async () => { calls.push('scan'); return { ok: true, action: 'scan', id: occupancyFrameIdForScan(scanBytes) }; },
      saveOccupancy: async () => { calls.push('occupancy'); return { ok: true, action: 'occupancy' }; },
      savePreview: async () => { calls.push('preview'); return { ok: true, action: 'preview' }; },
      fetchInfo: async () => ({ present: false, bytes: 0 } as ScanArtifactInfo),
    };
    return { ports, calls };
  }

  it('uploads the scan before a held asynchronous occupancy builder resolves, then uploads the awaited bytes', async () => {
    const { ports, calls } = fakePorts();
    const controller = new ScanPersistenceController(ports);
    let release!: (bytes: Uint8Array) => void;
    const held = new Promise<Uint8Array>((resolve) => { release = resolve; });
    const pending = controller.save('OBJ', 'SESSION', scanBytes, () => held, () => previewBytes);
    for (let i = 0; i < 100; i += 1) await Promise.resolve();
    expect(calls).toEqual(['scan']);
    expect(controller.currentState).toMatchObject({ scan: 'saved', occupancy: 'pending' });
    release(occupancyBytes);
    const state = await pending;
    expect(calls).toEqual(['scan', 'occupancy', 'preview']);
    expect(state).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
  });

  it('reports an explicit occupancy failure when the asynchronous builder rejects, and still builds the preview', async () => {
    const { ports, calls } = fakePorts();
    const controller = new ScanPersistenceController(ports);
    const state = await controller.save('OBJ', 'SESSION', scanBytes, () => Promise.reject(new Error('registration unavailable')), () => previewBytes);
    expect(calls).toEqual(['scan', 'preview']);
    expect(state).toMatchObject({ scan: 'saved', occupancy: 'failed', preview: 'ready' });
    expect(state?.occupancyError).toContain('registration unavailable');
  });
});

describe('scan persistence ownership and owner invalidation (MAX-392/MAX-415)', () => {
  it('releases owned scan bytes and prevents late info from reviving an abandoned owner', async () => {
    const sourceBytes = new Uint8Array([7, 8, 9]);
    let finishInfo!: (info: ScanArtifactInfo) => void;
    const infoPending = new Promise<ScanArtifactInfo>((resolve) => { finishInfo = resolve; });
    const controller = new ScanPersistenceController({
      saveScan: async () => ({ ok: true, action: 'scan' }),
      saveOccupancy: async () => ({ ok: true, action: 'occupancy' }),
      savePreview: async () => ({ ok: true, action: 'preview' }),
      fetchInfo: async () => infoPending,
    });
    const saving = controller.save('OBJ', 'SESSION', sourceBytes, () => new Uint8Array([1]), () => new Uint8Array([2]));
    const pending = (controller as unknown as { pendingScan: { bytes: Uint8Array | null; buildOccupancy: () => unknown } | null }).pendingScan;

    expect(pending).not.toBeNull();
    expect(pending?.bytes).not.toBe(sourceBytes);
    controller.abandon();
    expect(pending?.bytes).toBeNull();
    expect(() => pending?.buildOccupancy()).toThrow(/owner was abandoned/);
    finishInfo({ present: false, bytes: 0, id: null, sourceId: null });
    await expect(saving).resolves.toMatchObject({ scan: 'saving' });
    expect((controller as unknown as { knownScan: unknown }).knownScan).toBeNull();
  });

  it('rejects a loaded scan ID that disagrees with its immutable identity', async () => {
    const bytes = new Uint8Array([17, 18, 19]);
    const identity = createScanIdentity(bytes);
    let builderCalls = 0;
    const controller = new ScanPersistenceController({
      saveScan: async () => ({ ok: true, action: 'scan' }),
      saveOccupancy: async () => ({ ok: true, action: 'occupancy' }),
      savePreview: async () => ({ ok: true, action: 'preview' }),
      fetchInfo: async (_objectId, _sessionId, kind) => kind === 'scan'
        ? { present: true, bytes: identity.byteLength, id: identity.frameId, sourceId: null }
        : { present: false, bytes: 0, id: null, sourceId: null },
    });

    const state = await controller.reconcile(
      'OBJ', 'SESSION', bytes,
      () => { builderCalls += 1; return new Uint8Array([1]); },
      () => { builderCalls += 1; return new Uint8Array([2]); },
      'scan-crc32:ffffffff',
      identity,
    );

    expect(state).toMatchObject({ scan: 'unknown', scanError: expect.stringContaining('do not match') });
    expect(builderCalls).toBe(0);
  });

  it('keeps one owned scan payload and adopts derivative buffers without copying', async () => {
    const sourceBytes = new Uint8Array([11, 22, 33, 44]);
    const identity = createScanIdentity(sourceBytes);
    resetScanIdentityMetrics();
    const occupancyBytes = new Uint8Array([51, 52]);
    const previewBytes = new Uint8Array([61, 62, 63]);
    let uploadedScan: Uint8Array | null = null;
    let uploadedOccupancy: Uint8Array | null = null;
    let uploadedPreview: Uint8Array | null = null;
    const controller = new ScanPersistenceController({
      saveScan: async (_objectId, _sessionId, bytes) => {
        uploadedScan = bytes;
        return { ok: true, action: 'scan', id: identity.frameId };
      },
      saveOccupancy: async (_objectId, _sessionId, bytes) => {
        uploadedOccupancy = bytes;
        return { ok: true, action: 'occupancy' };
      },
      savePreview: async (_objectId, _sessionId, bytes) => {
        uploadedPreview = bytes;
        return { ok: true, action: 'preview' };
      },
      fetchInfo: async () => ({ present: false, bytes: 0, id: null, sourceId: null }),
    });

    const state = await controller.save(
      'OBJ', 'SESSION', sourceBytes, () => occupancyBytes, () => previewBytes, false, identity.frameId, identity,
    );

    expect(state).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
    expect(uploadedScan).not.toBe(sourceBytes);
    expect(uploadedScan?.buffer).not.toBe(sourceBytes.buffer);
    expect(uploadedOccupancy).toBe(occupancyBytes);
    expect(uploadedPreview).toBe(previewBytes);
    expect(getScanIdentityMetrics()).toEqual({
      wholeScanCrcInvocations: 0,
      wholeScanCrcBytesVisited: 0,
      persistenceBytesCopied: sourceBytes.byteLength,
    });
    expect(controller.retryAvailable).toBe(false);
  });

  it('does not retain loaded bytes in a reconciliation descriptor', async () => {
    const savedBytes = new Uint8Array([71, 72, 73]);
    const identity = createScanIdentity(savedBytes);
    let releaseOccupancy!: (bytes: Uint8Array) => void;
    let signalBuilderStarted!: () => void;
    const builderStarted = new Promise<void>((resolve) => { signalBuilderStarted = resolve; });
    const held = new Promise<Uint8Array>((resolve) => { releaseOccupancy = resolve; });
    const controller = new ScanPersistenceController({
      saveScan: async () => ({ ok: true, action: 'scan' }),
      saveOccupancy: async () => ({ ok: true, action: 'occupancy' }),
      savePreview: async () => ({ ok: true, action: 'preview' }),
      fetchInfo: async (_objectId, _sessionId, kind) => kind === 'scan'
        ? { present: true, bytes: identity.byteLength, id: identity.frameId }
        : { present: false, bytes: 0, id: null, sourceId: null },
    });
    const reconciling = controller.reconcile(
      'OBJ', 'SESSION', savedBytes,
      () => { signalBuilderStarted(); return held; },
      () => new Uint8Array([81]),
      identity.frameId,
      identity,
    );
    await builderStarted;
    const descriptor = (controller as unknown as { pendingScan: { bytes: Uint8Array | null; identity: unknown } | null }).pendingScan;

    expect(descriptor?.bytes).toBeNull();
    expect(descriptor?.identity).toBe(identity);
    expect(descriptor !== null && Object.values(descriptor).includes(savedBytes)).toBe(false);
    releaseOccupancy(new Uint8Array([91]));
    await reconciling;
  });

  it('abandons A derivatives after its primary commit when B replaces the scan', async () => {
    const server = await startControlServer();
    server.acceptOccupancy = true;
    try {
      const bytesA = new Uint8Array([101, 1, 1]);
      const bytesB = new Uint8Array([102, 2, 2]);
      let releaseOccupancyA!: (bytes: Uint8Array) => void;
      let signalBuilderStarted!: () => void;
      const builderStarted = new Promise<void>((resolve) => { signalBuilderStarted = resolve; });
      const heldOccupancyA = new Promise<Uint8Array>((resolve) => { releaseOccupancyA = resolve; });
      const controllerA = new ScanPersistenceController(server.ports);
      const controllerB = new ScanPersistenceController(server.ports);
      const savingA = controllerA.save('OBJ_ABANDON', 'S000001', bytesA, () => {
        signalBuilderStarted();
        return heldOccupancyA;
      }, () => new Uint8Array([1]));

      await builderStarted;
      expect(server.files.get('OBJ_ABANDON/S000001/scan')).toEqual(bytesA);
      controllerA.abandon();
      const identityB = createScanIdentity(bytesB);
      const savedB = await controllerB.save(
        'OBJ_ABANDON', 'S000001', bytesB,
        () => new Uint8Array([2]),
        () => new Uint8Array([3]),
        false,
        identityB.frameId,
        identityB,
      );
      releaseOccupancyA(new Uint8Array([4]));
      await savingA;

      expect(savedB).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
      expect(server.files.get('OBJ_ABANDON/S000001/scan')).toEqual(bytesB);
      expect(server.occupancyUploads).toEqual([new Uint8Array([2])]);
      expect(server.previewUploads).toEqual([new Uint8Array([3])]);
      expect(server.derivativeScanIds).toEqual([identityB.frameId, identityB.frameId]);
    } finally {
      await server.close();
    }
  });

  it('rejects controller A derivatives with 409 after controller B replaces the scan generation', async () => {
    const server = await startControlServer();
    server.acceptOccupancy = true;
    try {
      const objectId = 'OBJ_GENERATION';
      const sessionId = 'S000002';
      const bytesA = new Uint8Array([111, 1, 1]);
      const bytesB = new Uint8Array([112, 2, 2]);
      const identityA = createScanIdentity(bytesA);
      const identityB = createScanIdentity(bytesB);
      let releaseOccupancyA!: (bytes: Uint8Array) => void;
      let signalBuilderStarted!: () => void;
      const builderStarted = new Promise<void>((resolve) => { signalBuilderStarted = resolve; });
      const heldOccupancyA = new Promise<Uint8Array>((resolve) => { releaseOccupancyA = resolve; });
      const controllerA = new ScanPersistenceController(server.ports);
      const controllerB = new ScanPersistenceController(server.ports);
      const savingA = controllerA.save(
        objectId,
        sessionId,
        bytesA,
        () => { signalBuilderStarted(); return heldOccupancyA; },
        () => new Uint8Array([5]),
        false,
        identityA.frameId,
        identityA,
      );

      await builderStarted;
      expect(server.files.get(`${objectId}/${sessionId}/scan`)).toEqual(bytesA);
      const savedB = await controllerB.save(
        objectId,
        sessionId,
        bytesB,
        () => new Uint8Array([2]),
        () => new Uint8Array([3]),
        false,
        identityB.frameId,
        identityB,
      );
      releaseOccupancyA(new Uint8Array([4]));
      const staleA = await savingA;

      expect(savedB).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
      expect(staleA).toMatchObject({
        scan: 'derived_stale',
        occupancy: 'stale',
        preview: 'stale',
        scanError: expect.stringContaining('scan_generation_mismatch'),
      });
      expect(server.files.get(`${objectId}/${sessionId}/scan`)).toEqual(bytesB);
      expect(server.files.get(`${objectId}/${sessionId}/occupancy`)).toEqual(new Uint8Array([2]));
      expect(server.files.get(`${objectId}/${sessionId}/preview`)).toEqual(new Uint8Array([3]));
      expect(server.occupancyUploads).toEqual([new Uint8Array([2]), new Uint8Array([4])]);
      expect(server.previewUploads).toEqual([new Uint8Array([3])]);
      expect(server.derivativeScanIds).toEqual([identityB.frameId, identityB.frameId, identityA.frameId]);
      expect(controllerA.retryAvailable).toBe(false);
    } finally {
      await server.close();
    }
  });
});
