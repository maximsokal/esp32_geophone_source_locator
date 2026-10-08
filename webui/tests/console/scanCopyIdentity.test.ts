import { afterEach, describe, expect, it, vi } from 'vitest';

import { saveObjectScanArtifact, sendControl, type ScanArtifactInfo } from '../../src/api/deviceApi';
import {
  decodeScanArtifactWithIdentity,
  encodeScanArtifactWithIdentity,
  getScanArtifactMetrics,
  MAX_SCAN_ARTIFACT_BYTES,
  resetScanArtifactMetrics,
} from '../../src/xr/scanArtifact';
import { getScanIdentityMetrics, resetScanIdentityMetrics } from '../../src/xr/scanIdentity';
import { ScanPersistenceController, type ScanPersistencePorts } from '../../src/xr/scanPersistence';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
}

function nearLimitSnapshot(): ScanPointCloudSnapshot {
  const snapshot = makeValidScanSnapshot();
  delete snapshot.normalConfidence;
  snapshot.pointCount = 299_000;
  snapshot.rawPointCount = snapshot.pointCount;
  snapshot.maxPointCapacity = snapshot.pointCount;
  snapshot.memoryBudgetPoints = snapshot.pointCount;
  snapshot.positions = new Float32Array(snapshot.pointCount * 3);
  snapshot.confidence = new Float32Array(snapshot.pointCount).fill(0.8);
  snapshot.normals = new Float32Array(snapshot.pointCount * 3);
  return snapshot;
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((complete) => { resolve = complete; });
  return { promise, resolve };
}

function referencesLoadedBytes(value: unknown, bytes: Uint8Array, visited = new Set<object>()): boolean {
  if (value === bytes || value === bytes.buffer) return true;
  if (ArrayBuffer.isView(value)) return value.buffer === bytes.buffer;
  if (typeof value !== 'object' || value === null || visited.has(value)) return false;
  visited.add(value);
  return Reflect.ownKeys(value).some((key) => referencesLoadedBytes((value as Record<PropertyKey, unknown>)[key], bytes, visited));
}

afterEach(() => vi.unstubAllGlobals());

describe('MAX-392 scan copy and identity contract', () => {
  it('accounts encode through controller save and API transport for a near-limit artifact', async () => {
    resetScanArtifactMetrics();
    resetScanIdentityMetrics();
    const snapshot = nearLimitSnapshot();
    const encoded = encodeScanArtifactWithIdentity(snapshot);
    const payloadLength = new DataView(encoded.bytes.buffer).getUint32(12, true);
    const encodeMetrics = getScanArtifactMetrics();

    expect(encoded.bytes.byteLength).toBeGreaterThan(MAX_SCAN_ARTIFACT_BYTES * 0.99);
    expect(encoded.bytes.byteLength).toBeLessThanOrEqual(MAX_SCAN_ARTIFACT_BYTES);
    expect(encodeMetrics.encoderArrayBytesCopied).toBe(payloadLength);
    expect(getScanIdentityMetrics()).toMatchObject({
      wholeScanCrcInvocations: 1,
      wholeScanCrcBytesVisited: encoded.bytes.byteLength,
    });
    console.info('MAX-392 near-limit encode metrics', JSON.stringify({
      artifactBytes: encoded.bytes.byteLength,
      floatsVisited: (encodeMetrics.validationBytesVisited - snapshot.tsdfCoverageReady.byteLength) / Float32Array.BYTES_PER_ELEMENT,
      bytesCopied: encodeMetrics.encoderArrayBytesCopied,
      artifactChecksumCrcBytes: encodeMetrics.artifactChecksumCrcBytesVisited,
      wholeScanCrcBytes: getScanIdentityMetrics().wholeScanCrcBytesVisited,
    }));

    let uploadedBody: ArrayBuffer | null = null;
    let controllerBuffer: ArrayBuffer | null = null;
    vi.stubGlobal('fetch', vi.fn(async (url: string, init?: RequestInit) => {
      expect(url).toContain('scan=save');
      uploadedBody = init?.body as ArrayBuffer;
      controllerBuffer = (controller as unknown as { pendingScan: { bytes: Uint8Array } }).pendingScan.bytes.buffer as ArrayBuffer;
      return jsonResponse({
        ok: true,
        action: 'scan_save',
        bytes: encoded.bytes.byteLength,
        id: encoded.identity.frameId,
      });
    }));
    const ports: ScanPersistencePorts = {
      saveScan: saveObjectScanArtifact,
      saveOccupancy: async () => ({ ok: true, action: 'scan_save' }),
      savePreview: async () => ({ ok: true, action: 'scan_save' }),
      fetchInfo: async () => ({ present: false, bytes: 0, id: null, sourceId: null }),
    };
    const controller = new ScanPersistenceController(ports);

    const state = await controller.save(
      'OBJ_0392', 'S000392', encoded.bytes, () => new Uint8Array([1]), () => new Uint8Array([2]),
      false, encoded.identity.frameId, encoded.identity,
    );

    expect(state).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
    expect(uploadedBody).toBeInstanceOf(ArrayBuffer);
    expect(uploadedBody).toBe(controllerBuffer);
    expect(uploadedBody).not.toBe(encoded.bytes.buffer);
    expect(Array.from(new Uint8Array(uploadedBody as ArrayBuffer))).toEqual(Array.from(encoded.bytes));
    expect(getScanIdentityMetrics()).toMatchObject({
      wholeScanCrcInvocations: 1,
      wholeScanCrcBytesVisited: encoded.bytes.byteLength,
      controllerOwnedCopies: 1,
      controllerOwnedBytesCopied: encoded.bytes.byteLength,
      apiBoundaryCopies: 0,
      apiBoundaryBytesCopied: 0,
      persistenceBytesCopied: encoded.bytes.byteLength,
    });
  });

  it('retains only compact reconciliation state while fetchInfo is deferred after decode', async () => {
    const sourceBytes = encodeScanArtifactWithIdentity(nearLimitSnapshot()).bytes;
    const loaded = decodeScanArtifactWithIdentity(sourceBytes);
    const pendingInfo = {
      scan: deferred<ScanArtifactInfo>(),
      occupancy: deferred<ScanArtifactInfo>(),
      preview: deferred<ScanArtifactInfo>(),
    };
    const occupancyBytes = deferred<Uint8Array>();
    let signalOccupancyBuilderStarted!: () => void;
    const occupancyBuilderStarted = new Promise<void>((resolve) => { signalOccupancyBuilderStarted = resolve; });
    let infoCalls = 0;
    let releaseAllStarted!: () => void;
    const allStarted = new Promise<void>((resolve) => { releaseAllStarted = resolve; });
    const controller = new ScanPersistenceController({
      saveScan: async () => ({ ok: true, action: 'scan_save', id: loaded.identity.frameId }),
      saveOccupancy: async () => ({ ok: true, action: 'scan_save' }),
      savePreview: async () => ({ ok: true, action: 'scan_save' }),
      fetchInfo: async (_objectId, _sessionId, kind) => {
        infoCalls += 1;
        if (infoCalls === 3) releaseAllStarted();
        return pendingInfo[kind].promise;
      },
    });
    const reconciling = controller.reconcile(
      'OBJ_0392', 'S000392', sourceBytes,
      () => {
        signalOccupancyBuilderStarted();
        return occupancyBytes.promise;
      },
      () => new Uint8Array([2]),
      loaded.identity.frameId, loaded.identity,
    );
    await allStarted;

    expect(infoCalls).toBe(3);
    expect((controller as unknown as { pendingScan: unknown }).pendingScan).toBeNull();
    expect(referencesLoadedBytes(controller, sourceBytes)).toBe(false);

    pendingInfo.scan.resolve({ present: true, bytes: sourceBytes.byteLength, id: loaded.identity.frameId, sourceId: null });
    pendingInfo.occupancy.resolve({ present: false, bytes: 0, id: null, sourceId: null });
    pendingInfo.preview.resolve({ present: false, bytes: 0, id: null, sourceId: null });
    await occupancyBuilderStarted;
    const descriptor = (controller as unknown as {
      pendingScan: { bytes: Uint8Array | null; identity: unknown } | null;
    }).pendingScan;
    expect(descriptor?.bytes).toBeNull();
    expect(descriptor?.identity).toBe(loaded.identity);
    expect(referencesLoadedBytes(controller, sourceBytes)).toBe(false);
    occupancyBytes.resolve(new Uint8Array([1]));
    await reconciling;
  });

  it('preserves the retry payload when critical preemption aborts a fetch request', async () => {
    resetScanIdentityMetrics();
    const encoded = encodeScanArtifactWithIdentity(makeValidScanSnapshot());
    let controller!: ScanPersistenceController;
    let firstSignal!: AbortSignal;
    let signalFirstSaveStarted!: () => void;
    const firstSaveStarted = new Promise<void>((resolve) => { signalFirstSaveStarted = resolve; });
    let retryPayloadAtFirstFetch: Uint8Array | null = null;
    const requestBodySharesRetryStorage: boolean[] = [];
    const uploaded: number[][] = [];
    let saveAttempts = 0;
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      if (url.includes('scan=save')) {
        saveAttempts += 1;
        const body = init?.body;
        const bytes = body instanceof ArrayBuffer
          ? new Uint8Array(body).slice()
          : ArrayBuffer.isView(body)
            ? new Uint8Array(body.buffer, body.byteOffset, body.byteLength).slice()
            : null;
        expect(bytes).not.toBeNull();
        uploaded.push(Array.from(bytes ?? []));
        const retryPayload = (controller as unknown as {
          pendingScan: { bytes: Uint8Array };
        }).pendingScan.bytes;
        requestBodySharesRetryStorage.push(
          body === retryPayload.buffer || (ArrayBuffer.isView(body) && body.buffer === retryPayload.buffer),
        );
        if (saveAttempts === 1) {
          firstSignal = init?.signal as AbortSignal;
          retryPayloadAtFirstFetch = retryPayload;
          signalFirstSaveStarted();
          return new Promise<Response>((_resolve, reject) => {
            firstSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
          });
        }
        return Promise.resolve(jsonResponse({
          ok: true,
          action: 'scan_save',
          bytes: encoded.bytes.byteLength,
          id: encoded.identity.frameId,
        }));
      }
      return Promise.resolve(jsonResponse({ ok: true, action: 'measurement_stop' }));
    });
    vi.stubGlobal('fetch', fetchMock);
    const ports: ScanPersistencePorts = {
      saveScan: saveObjectScanArtifact,
      saveOccupancy: async () => ({ ok: true, action: 'scan_save' }),
      savePreview: async () => ({ ok: true, action: 'scan_save' }),
      fetchInfo: async () => ({ present: false, bytes: 0, id: null, sourceId: null }),
    };
    controller = new ScanPersistenceController(ports);

    const saving = controller.save(
      'OBJ_0392', 'S000393', encoded.bytes, () => new Uint8Array([1]), () => new Uint8Array([2]),
      false, encoded.identity.frameId, encoded.identity,
    );
    await firstSaveStarted;
    const stop = sendControl('measurement_stop');
    const [paused] = await Promise.all([saving, stop]);

    expect(requestBodySharesRetryStorage[0]).toBe(true);
    expect(retryPayloadAtFirstFetch).not.toBeNull();
    expect(retryPayloadAtFirstFetch?.byteLength).toBe(encoded.bytes.byteLength);
    expect(Array.from(retryPayloadAtFirstFetch ?? [])).toEqual(Array.from(encoded.bytes));
    expect(uploaded[0]).toEqual(Array.from(encoded.bytes));
    expect(paused).toMatchObject({ scan: 'error', outcome: 'confirmed_not_committed' });
    expect(controller.retryAvailable).toBe(true);

    const retried = await controller.retry('OBJ_0392', 'S000393');

    expect(retried).toMatchObject({ scan: 'saved', occupancy: 'ready', preview: 'ready' });
    expect(uploaded).toEqual([Array.from(encoded.bytes), Array.from(encoded.bytes)]);
    expect(requestBodySharesRetryStorage).toEqual([true, true]);
    expect(Array.from(retryPayloadAtFirstFetch ?? [])).toEqual(uploaded[1]);
    expect(saveAttempts).toBe(2);
    expect(getScanIdentityMetrics()).toMatchObject({
      wholeScanCrcInvocations: 1,
      controllerOwnedCopies: 1,
      apiBoundaryCopies: 0,
      apiBoundaryBytesCopied: 0,
      persistenceBytesCopied: encoded.bytes.byteLength,
    });
  });
});
