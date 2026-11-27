import { describe, expect, it, vi } from 'vitest';

import {
  ScanEncodeBudgetExceededError,
  ScanEncodeCancelledError,
  ScanEncodeClient,
  SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES,
} from '../../src/xr/scanEncodeClient';
import { buildSessionOccupancyArtifact } from '../../src/xr/occupancySolverArtifact';
import {
  encodeScanArtifactWithIdentity,
  SCAN_ANNOTATION_CAPACITY_ERROR,
} from '../../src/xr/scanArtifact';
import { getScanIdentityMetrics, resetScanIdentityMetrics } from '../../src/xr/scanIdentity';
import { buildVoxelPreviewArtifact } from '../../src/xr/voxelPreview';
import { processScanEncodeWorkerMessage, type ScanEncodeWorkerRequest, type ScanEncodeWorkerResponse } from '../../src/xr/scanEncodeWorker';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from './support/validScanSnapshot';

/**
 * Byte-exact comparison of two artifacts. `expect(a).toEqual(b)` on an 8 MiB Uint8Array walks the arrays element by element through the generic deep-equal
 * (18 s for the near-limit fixture, the cause of the timeouts under load; the encode itself takes 85 ms), so the bytes are compared natively and the first
 * differing offset is reported on failure.
 */
function expectSameBytes(actual: Uint8Array, expected: Uint8Array): void {
  expect(actual.byteLength, 'artifact size').toBe(expected.byteLength);
  const a = Buffer.from(actual.buffer, actual.byteOffset, actual.byteLength);
  const b = Buffer.from(expected.buffer, expected.byteOffset, expected.byteLength);
  if (Buffer.compare(a, b) === 0) return;
  let offset = 0;
  while (a[offset] === b[offset]) offset += 1;
  expect.fail(`artifact bytes differ at offset ${offset}: ${a[offset]} vs ${b[offset]}`);
}

type WorkerListener = (event: MessageEvent<ScanEncodeWorkerResponse> | ErrorEvent) => void;

class DeferredWorker {
  readonly listeners = new Map<string, WorkerListener>();
  request: ScanEncodeWorkerRequest | null = null;
  transfer: Transferable[] = [];
  terminated = false;

  addEventListener(type: string, listener: WorkerListener): void { this.listeners.set(type, listener); }
  removeEventListener(type: string): void { this.listeners.delete(type); }
  postMessage(message: unknown, transfer: Transferable[] = []): void {
    this.request = message as ScanEncodeWorkerRequest;
    this.transfer = transfer;
  }
  terminate(): void { this.terminated = true; }
  send(data: ScanEncodeWorkerResponse): void { this.listeners.get('message')?.({ data } as MessageEvent<ScanEncodeWorkerResponse>); }
  fail(message: string): void { this.listeners.get('error')?.({ message } as ErrorEvent); }
}

class InProcessScanEncodeWorker extends DeferredWorker {
  postMessage(message: unknown, transfer: Transferable[] = []): void {
    super.postMessage(message, transfer);
    const request = structuredClone(message, { transfer }) as ScanEncodeWorkerRequest;
    queueMicrotask(() => {
      if (this.terminated) return;
      processScanEncodeWorkerMessage(request, {
        postMessage: (response, responseTransfer = []) => {
          this.send(structuredClone(response, { transfer: responseTransfer }) as ScanEncodeWorkerResponse);
        },
      });
    });
  }
}

function nearLimitSnapshot(): ScanPointCloudSnapshot {
  const snapshot = makeValidScanSnapshot();
  const pointCount = 260_000;
  const bounds = { min: [0, 0, 0] as const, max: [0, 0, 0] as const };
  return {
    ...snapshot,
    positions: new Float32Array(pointCount * 3),
    confidence: new Float32Array(pointCount).fill(0.9),
    normals: new Float32Array(pointCount * 3),
    normalConfidence: new Float32Array(pointCount).fill(0.85),
    pointCount,
    rawPointCount: pointCount,
    maxPointCapacity: pointCount,
    memoryBudgetPoints: pointCount,
    bounds,
    fullBounds: bounds,
    freeAirPositions: new Float32Array(0),
    occupiedSurfacePositions: new Float32Array(0),
    unknownCandidatePositions: new Float32Array(0),
    knownFreeVoxelCount: 0,
    occupiedSurfaceVoxelCount: 0,
    unknownCandidateVoxelCount: 0,
  };
}

function legacyFieldSnapshot(): ScanPointCloudSnapshot {
  const snapshot = makeValidScanSnapshot();
  delete snapshot.normalConfidence;
  return snapshot;
}

function withOneTsdfPoint(snapshot: ScanPointCloudSnapshot): ScanPointCloudSnapshot {
  return {
    ...snapshot,
    tsdfPositions: new Float32Array([0.1, 0.2, 0.3]),
    tsdfNormals: new Float32Array([0, 0, 1]),
    tsdfConfidence: new Float32Array([0.9]),
    tsdfResidualMeters: new Float32Array([0.01]),
    tsdfNormalConfidence: new Float32Array([0.9]),
    tsdfCoverageConfidence: new Float32Array([0.9]),
    tsdfCoverageReady: new Uint8Array([1]),
    tsdfPointCount: 1,
  };
}

function invalidFloatSnapshot(field: keyof ScanPointCloudSnapshot): ScanPointCloudSnapshot {
  let snapshot = makeValidScanSnapshot();
  if (field.startsWith('tsdf')) snapshot = withOneTsdfPoint(snapshot);
  if (field === 'tentativePositions' || field === 'tentativeConfidence') {
    snapshot = { ...snapshot, tentativePositions: new Float32Array([0.1, 0.2, 0.3]), tentativeConfidence: new Float32Array([0.8]), tentativePointCount: 1 };
  }
  if (field === 'normalConfidence') snapshot.normalConfidence = new Float32Array([0.8]);
  const array = snapshot[field];
  if (!(array instanceof Float32Array)) throw new Error(`${String(field)} is not a persisted float array in the fixture.`);
  array[0] = Number.NaN;
  return snapshot;
}

function clientFor(worker: DeferredWorker): ScanEncodeClient {
  return new ScanEncodeClient(() => worker as unknown as Worker);
}

describe('ScanEncodeClient', () => {
  it('keeps the main thread request pending and transfers only copies of the source arrays', async () => {
    const worker = new DeferredWorker();
    const client = clientFor(worker);
    const snapshot = makeValidScanSnapshot();
    const originalBuffer = snapshot.positions.buffer;
    let settled = false;
    const pending = client.encode(snapshot).finally(() => { settled = true; });
    await Promise.resolve();

    expect(settled).toBe(false);
    expect(worker.request?.type).toBe('encode');
    expect(worker.request?.snapshot.positions).not.toBe(snapshot.positions);
    expect(worker.transfer).toContain(worker.request?.snapshot.positions.buffer);
    expect(worker.request?.copyBytes).toBeGreaterThan(0);
    expect(snapshot.positions.buffer).toBe(originalBuffer);
    expect(snapshot.positions.byteLength).toBeGreaterThan(0);

    client.cancel('test cancellation');
    await expect(pending).rejects.toBeInstanceOf(ScanEncodeCancelledError);
    expect(worker.terminated).toBe(true);
  });

  it.each([
    ['near-limit', nearLimitSnapshot],
    ['annotations', makeValidScanSnapshot],
    ['legacy optional fields', legacyFieldSnapshot],
  ])('matches synchronous bytes and identity for the %s fixture', async (_name, makeSnapshot) => {
    const snapshot = makeSnapshot();
    const sync = encodeScanArtifactWithIdentity(snapshot);
    const worker = new InProcessScanEncodeWorker();
    const client = clientFor(worker);

    const encoded = await client.encode(snapshot, { generation: 17 });

    expectSameBytes(encoded.bytes, sync.bytes);
    expect(encoded.identity).toEqual(sync.identity);
    expect(encoded.counters.bytesCopied).toBe(worker.request?.copyBytes);
    expect(encoded.counters.wholeScanCrcInvocations).toBe(1);
    expect(encoded.counters.wholeScanCrcBytesVisited).toBe(sync.bytes.byteLength);
    expect(encoded.counters.floatsVisited).toBeGreaterThan(0);
  });

  it('the native byte comparison still sees a single changed byte, a truncated artifact and an identical copy', () => {
    const original = encodeScanArtifactWithIdentity(nearLimitSnapshot()).bytes;
    expect(() => expectSameBytes(original.slice(), original)).not.toThrow();
    const flipped = original.slice(); flipped[flipped.length - 3] ^= 1;
    expect(() => expectSameBytes(flipped, original)).toThrow(/differ at offset/);
    expect(() => expectSameBytes(original.slice(0, original.length - 1), original)).toThrow();
  });

  it('returns encode counters once and reuses the identity in occupancy and preview builders', async () => {
    resetScanIdentityMetrics();
    const snapshot = makeValidScanSnapshot();
    const encoded = await clientFor(new InProcessScanEncodeWorker()).encode(snapshot);

    expect(encoded.counters.wholeScanCrcInvocations).toBe(1);
    expect(encoded.counters.bytesCopied).toBeGreaterThan(0);
    expect(encoded.counters.crcBytesVisited).toBeGreaterThan(encoded.counters.wholeScanCrcBytesVisited);
    const occupancy = buildSessionOccupancyArtifact(snapshot, encoded.identity, Date.now());
    buildVoxelPreviewArtifact(snapshot, encoded.identity, {
      occupancyArtifact: occupancy,
      identity: encoded.identity,
    });

    expect(getScanIdentityMetrics().wholeScanCrcInvocations).toBe(1);
  });

  it.each([
    'positions', 'confidence', 'normals', 'tentativePositions', 'tentativeConfidence',
    'freeAirPositions', 'occupiedSurfacePositions', 'unknownCandidatePositions', 'tsdfPositions',
    'tsdfNormals', 'tsdfConfidence', 'tsdfResidualMeters', 'tsdfNormalConfidence',
    'tsdfCoverageConfidence', 'normalConfidence',
  ] as const)('fails closed when persisted float field %s contains NaN', async (field) => {
    const client = clientFor(new InProcessScanEncodeWorker());
    await expect(client.encode(invalidFloatSnapshot(field))).rejects.toThrow(new RegExp(field));
  });

  it('preserves the annotation capacity error from the worker', async () => {
    const snapshot = makeValidScanSnapshot();
    snapshot.manualControlPoints = Array.from({ length: 257 }, (_, index) => ({ id: index + 1, position: [0, 0, 0] as const }));
    await expect(clientFor(new InProcessScanEncodeWorker()).encode(snapshot)).rejects.toThrow(SCAN_ANNOTATION_CAPACITY_ERROR);
  });

  it('rejects a worker error, terminates the worker, and does not return a partial artifact', async () => {
    const worker = new DeferredWorker();
    const pending = clientFor(worker).encode(makeValidScanSnapshot());
    worker.fail('worker failed');

    await expect(pending).rejects.toThrow('worker failed');
    expect(worker.terminated).toBe(true);
  });

  it('drops a late stale artifact and detaches its result bytes after a newer Finish', async () => {
    const workers: DeferredWorker[] = [];
    const client = new ScanEncodeClient(() => {
      const worker = new DeferredWorker();
      workers.push(worker);
      return worker as unknown as Worker;
    });
    const first = client.encode(makeValidScanSnapshot(), { generation: 1 });
    const firstWorker = workers[0];
    const lateHandler = firstWorker.listeners.get('message');
    const firstRejected = expect(first).rejects.toBeInstanceOf(ScanEncodeCancelledError);

    const second = client.encode(makeValidScanSnapshot(), { generation: 2 });
    const staleBytes = new Uint8Array([1, 2, 3]);
    lateHandler?.({ data: {
      type: 'result', requestId: firstWorker.request!.requestId, generation: 1,
      artifact: staleBytes.buffer, identity: { byteLength: 3, crc32: 1, frameId: 'scan-crc32:00000001' },
      counters: { floatsVisited: 0, validationBytesVisited: 0, encoderArrayBytesCopied: 0,
        artifactChecksumCrcBytesVisited: 0, crcBytesVisited: 0, wholeScanCrcInvocations: 0,
        wholeScanCrcBytesVisited: 0, bytesCopied: 0 },
    } } as MessageEvent<ScanEncodeWorkerResponse>);

    expect(firstWorker.terminated).toBe(true);
    expect(firstWorker.request?.snapshot.positions.byteLength).toBe(0);
    expect(staleBytes.byteLength).toBe(0);
    await firstRejected;
    client.cancel('test cleanup');
    await expect(second).rejects.toBeInstanceOf(ScanEncodeCancelledError);
  });

  it('rejects an over-budget snapshot before creating a worker', async () => {
    const snapshot = makeValidScanSnapshot();
    snapshot.positions = new Float32Array(SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES + Float32Array.BYTES_PER_ELEMENT);
    const factory = vi.fn(() => new DeferredWorker() as unknown as Worker);
    const client = new ScanEncodeClient(factory);

    await expect(client.encode(snapshot)).rejects.toBeInstanceOf(ScanEncodeBudgetExceededError);
    expect(factory).not.toHaveBeenCalled();
  });

  it('logs through the explicit compatibility callback when Worker is unavailable', async () => {
    vi.stubGlobal('Worker', undefined);
    const onFallback = vi.fn();
    try {
      const result = await new ScanEncodeClient().encode(makeValidScanSnapshot(), { onFallback });
      expect(onFallback).toHaveBeenCalledTimes(1);
      expect(result.counters.bytesCopied).toBe(0);
      expect(result.identity.frameId).toMatch(/^scan-crc32:[0-9a-f]{8}$/);
    } finally {
      vi.unstubAllGlobals();
    }
  });
});
