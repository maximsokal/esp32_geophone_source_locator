import { describe, expect, it, vi } from 'vitest';

import {
  ScanDecodeBudgetExceededError,
  ScanDecodeCancelledError,
  ScanDecodeClient,
  SCAN_DECODE_TRANSIENT_BUDGET_BYTES,
  estimateScanDecodeTransientBytes,
} from '../../src/xr/scanDecodeClient';
import * as scanArtifact from '../../src/xr/scanArtifact';
import { decodeScanArtifactWithIdentity, encodeScanArtifact } from '../../src/xr/scanArtifact';
import {
  processScanDecodeWorkerMessage,
  type ScanDecodeWorkerRequest,
  type ScanDecodeWorkerResponse,
} from '../../src/xr/scanDecodeWorker';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { rewriteNormalConfidenceAsLegacy, rewriteScanArtifactMetadata } from './support/scanArtifactTestTools';
import { makeValidScanSnapshot } from './support/validScanSnapshot';

type WorkerListener = (event: MessageEvent<ScanDecodeWorkerResponse> | ErrorEvent) => void;

class DeferredWorker {
  readonly listeners = new Map<string, WorkerListener>();
  request: ScanDecodeWorkerRequest | null = null;
  transfer: Transferable[] = [];
  terminated = false;

  addEventListener(type: string, listener: WorkerListener): void { this.listeners.set(type, listener); }
  removeEventListener(type: string): void { this.listeners.delete(type); }
  postMessage(message: unknown, transfer: Transferable[] = []): void {
    this.request = message as ScanDecodeWorkerRequest;
    this.transfer = transfer;
  }
  terminate(): void { this.terminated = true; }
  send(data: ScanDecodeWorkerResponse): void { this.listeners.get('message')?.({ data } as MessageEvent<ScanDecodeWorkerResponse>); }
  fail(message: string): void { this.listeners.get('error')?.({ message } as ErrorEvent); }
}

class InProcessScanDecodeWorker extends DeferredWorker {
  postMessage(message: unknown, transfer: Transferable[] = []): void {
    super.postMessage(message, transfer);
    const request = structuredClone(message, { transfer }) as ScanDecodeWorkerRequest;
    queueMicrotask(() => {
      if (this.terminated) return;
      processScanDecodeWorkerMessage(request, {
        postMessage: (response, responseTransfer = []) => {
          this.send(structuredClone(response, { transfer: responseTransfer }) as ScanDecodeWorkerResponse);
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

function fixtureBytes(kind: 'near-limit' | 'annotations' | 'legacy-normal-confidence' | 'old-version-metadata'): Uint8Array {
  const snapshot = kind === 'near-limit' ? nearLimitSnapshot() : makeValidScanSnapshot();
  const bytes = encodeScanArtifact(snapshot);
  if (kind === 'legacy-normal-confidence') return rewriteNormalConfidenceAsLegacy(bytes, true);
  if (kind === 'old-version-metadata') {
    return rewriteScanArtifactMetadata(bytes, (metadata) => {
      delete metadata.occupancyFineVoxelSizeMeters;
      delete metadata.occupancyCellsSaturated;
      delete metadata.occupancyDroppedCellUpdates;
      delete metadata.occupancySolverCandidateBelowSurfaceMeters;
    });
  }
  return bytes;
}

function clientFor(worker: DeferredWorker): ScanDecodeClient {
  return new ScanDecodeClient(() => worker as unknown as Worker);
}

function workerError(bytes: Uint8Array): string {
  try {
    decodeScanArtifactWithIdentity(bytes);
  } catch (reason) {
    return reason instanceof Error ? reason.message : String(reason);
  }
  throw new Error('Expected the synchronous scan decoder to reject the fixture.');
}

describe('ScanDecodeClient', () => {
  it('keeps decode pending off the caller path and copies when the caller retains its artifact', async () => {
    const worker = new DeferredWorker();
    const decoderSpy = vi.spyOn(scanArtifact, 'decodeScanArtifactWithIdentityAndMetrics');
    const source = encodeScanArtifact(makeValidScanSnapshot());
    const sourceBuffer = source.buffer;
    let settled = false;
    const client = clientFor(worker);
    const pending = client.decode(source).finally(() => { settled = true; });
    await Promise.resolve();

    expect(settled).toBe(false);
    expect(decoderSpy).not.toHaveBeenCalled();
    expect(worker.request?.type).toBe('decode');
    expect(worker.request?.artifact).not.toBe(sourceBuffer);
    expect(worker.transfer).toContain(worker.request?.artifact);
    expect(source.byteLength).toBeGreaterThan(0);

    client.cancel('test cancellation');
    await expect(pending).rejects.toBeInstanceOf(ScanDecodeCancelledError);
    expect(worker.terminated).toBe(true);
    decoderSpy.mockRestore();
  });

  it('transfers a whole source buffer when the caller releases it', async () => {
    const worker = new DeferredWorker();
    const source = encodeScanArtifact(makeValidScanSnapshot());
    const sourceBuffer = source.buffer;
    const client = clientFor(worker);
    const pending = client.decode(source, { transferInput: true });

    expect(worker.request?.artifact).toBe(sourceBuffer);
    expect(worker.transfer).toContain(sourceBuffer);
    client.cancel('test cleanup');
    await expect(pending).rejects.toBeInstanceOf(ScanDecodeCancelledError);
  });

  it.each(['near-limit', 'annotations', 'legacy-normal-confidence', 'old-version-metadata'] as const)(
    'matches the synchronous snapshot and identity for the %s fixture', async (kind) => {
      const bytes = fixtureBytes(kind);
      const expected = decodeScanArtifactWithIdentity(bytes);
      const worker = new InProcessScanDecodeWorker();
      const decoded = await clientFor(worker).decode(bytes);

      expect(decoded.snapshot).toEqual(expected.snapshot);
      expect(decoded.identity).toEqual(expected.identity);
      expect(decoded.counters.wholeScanCrcInvocations).toBe(1);
      expect(decoded.counters.wholeScanCrcBytesVisited).toBe(bytes.byteLength);
      expect(decoded.counters.decoderPayloadBytesCopied).toBe(new DataView(bytes.buffer, bytes.byteOffset).getUint32(12, true));
      expect(decoded.counters.bytesCopied).toBe(bytes.byteLength);
      expect(worker.terminated).toBe(true);
      if (kind === 'near-limit') {
        expect(bytes.byteLength).toBeGreaterThan(scanArtifact.MAX_SCAN_ARTIFACT_BYTES * 0.9);
        expect(estimateScanDecodeTransientBytes(bytes)).toBeLessThanOrEqual(SCAN_DECODE_TRANSIENT_BUDGET_BYTES);
      }
    },
  );

  it('returns the same deterministic validation error for malformed metadata', async () => {
    const base = encodeScanArtifact(makeValidScanSnapshot());
    const malformed = rewriteScanArtifactMetadata(base, (metadata) => { metadata.pointCount = 'not-a-count'; });
    const expected = workerError(malformed);

    await expect(clientFor(new InProcessScanDecodeWorker()).decode(malformed)).rejects.toThrow(expected);
  });

  it('rejects CRC corruption with the same deterministic checksum error', async () => {
    const corrupted = encodeScanArtifact(makeValidScanSnapshot());
    corrupted[corrupted.length - 1] ^= 0x01;
    const expected = workerError(corrupted);

    await expect(clientFor(new InProcessScanDecodeWorker()).decode(corrupted)).rejects.toThrow(expected);
    expect(expected).toMatch(/checksum/i);
  });

  it('rejects over-budget input before creating a worker', async () => {
    const bytes = encodeScanArtifact(makeValidScanSnapshot());
    const estimate = estimateScanDecodeTransientBytes(bytes);
    const factory = vi.fn(() => new DeferredWorker() as unknown as Worker);
    const client = new ScanDecodeClient(factory);

    await expect(client.decode(bytes, { budgetLimitBytes: estimate - 1 })).rejects.toBeInstanceOf(ScanDecodeBudgetExceededError);
    expect(factory).not.toHaveBeenCalled();
  });

  it('terminates on cancellation and releases a late stale snapshot', async () => {
    const worker = new DeferredWorker();
    const client = clientFor(worker);
    const pending = client.decode(encodeScanArtifact(makeValidScanSnapshot()));
    const lateHandler = worker.listeners.get('message');
    const request = worker.request!;
    const firstRejected = expect(pending).rejects.toBeInstanceOf(ScanDecodeCancelledError);
    const decoded = decodeScanArtifactWithIdentity(new Uint8Array(request.artifact));
    const stale: ScanDecodeWorkerResponse = {
      type: 'result', requestId: request.requestId, generation: request.generation,
      snapshot: decoded.snapshot, identity: decoded.identity,
      counters: {
        floatsVisited: 0, validationBytesVisited: 0, decoderPayloadBytesCopied: 0,
        artifactChecksumCrcBytesVisited: 0, crcBytesVisited: 0, wholeScanCrcInvocations: 0,
        wholeScanCrcBytesVisited: 0, normalConfidenceValidationPasses: 0, bytesCopied: 0,
      },
    };
    const staleBuffer = decoded.snapshot.positions.buffer as ArrayBuffer;

    client.cancel('object changed');
    lateHandler?.({ data: stale } as MessageEvent<ScanDecodeWorkerResponse>);
    await firstRejected;

    expect(worker.terminated).toBe(true);
    expect(request.artifact.byteLength).toBe(0);
    expect(staleBuffer.byteLength).toBe(0);
  });

  it('rejects worker crashes without returning a partial decoded snapshot', async () => {
    const worker = new DeferredWorker();
    const pending = clientFor(worker).decode(encodeScanArtifact(makeValidScanSnapshot()));
    worker.fail('worker crashed');

    await expect(pending).rejects.toThrow('worker crashed');
    expect(worker.terminated).toBe(true);
  });

  it('logs through the explicit synchronous fallback when Worker is unavailable', async () => {
    vi.stubGlobal('Worker', undefined);
    const onFallback = vi.fn();
    try {
      const decoded = await new ScanDecodeClient().decode(encodeScanArtifact(makeValidScanSnapshot()), { onFallback });
      expect(onFallback).toHaveBeenCalledTimes(1);
      expect(decoded.identity.frameId).toMatch(/^scan-crc32:[0-9a-f]{8}$/);
      expect(decoded.counters.bytesCopied).toBe(0);
    } finally {
      vi.unstubAllGlobals();
    }
  });
});
