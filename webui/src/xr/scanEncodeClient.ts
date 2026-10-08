import ScanEncodeWorker from './scanEncodeWorker?worker&inline';
import {
  encodeScanArtifactWithIdentityAndMetrics,
  scanArtifactSnapshotArrayBytes,
  MAX_SCAN_ARTIFACT_BYTES,
} from './scanArtifact';
import type { ScanIdentity } from './scanIdentity';
import type { ScanPointCloudSnapshot } from './webxrRuntime';
import type {
  ScanEncodeCounters,
  ScanEncodeWorkerRequest,
  ScanEncodeWorkerResponse,
} from './scanEncodeWorker';

/** One full copy of the snapshot arrays is transient on the UI thread; cap it at the artifact size limit. */
export const SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES = MAX_SCAN_ARTIFACT_BYTES;

export interface ScanEncodeBudgetEstimate {
  estimatedBytes: number;
  limitBytes: number;
}

export interface ScanEncodeClientOptions {
  /** Owner generation supplied by App; stale owner responses are never accepted. */
  generation?: number;
  budgetLimitBytes?: number;
  onFallback?: () => void;
}

export interface ScanEncodeResult {
  bytes: Uint8Array;
  identity: ScanIdentity;
  counters: ScanEncodeCounters;
}

export class ScanEncodeBudgetExceededError extends Error {
  readonly code = 'scan_encode_transient_budget_exceeded';

  constructor(readonly estimate: ScanEncodeBudgetEstimate) {
    super(`scan_encode_transient_budget_exceeded: ${estimate.estimatedBytes} bytes estimated, ${estimate.limitBytes} byte limit`);
    this.name = 'ScanEncodeBudgetExceededError';
  }
}

export class ScanEncodeCancelledError extends Error {
  readonly code = 'scan_encode_cancelled';

  constructor(message = 'Scan encoding cancelled.') {
    super(message);
    this.name = 'ScanEncodeCancelledError';
  }
}

interface PendingScanEncode {
  requestId: number;
  generation: number;
  token: number;
  worker: Worker;
  copies: ScanPointCloudSnapshot | null;
  resolve: (result: ScanEncodeResult) => void;
  reject: (reason: unknown) => void;
}

type WorkerFactory = () => Worker;

function copySnapshot(snapshot: ScanPointCloudSnapshot): ScanPointCloudSnapshot {
  const copy = { ...snapshot };
  for (const [key, value] of Object.entries(snapshot)) {
    if (value instanceof Float32Array || value instanceof Uint8Array) {
      (copy as unknown as Record<string, unknown>)[key] = value.slice();
    }
  }
  return copy;
}

function snapshotBuffers(snapshot: ScanPointCloudSnapshot): ArrayBuffer[] {
  const buffers: ArrayBuffer[] = [];
  const seen = new Set<ArrayBuffer>();
  for (const value of Object.values(snapshot)) {
    if (!(value instanceof Float32Array || value instanceof Uint8Array)) continue;
    const buffer = value.buffer;
    if (buffer instanceof ArrayBuffer && !seen.has(buffer)) {
      seen.add(buffer);
      buffers.push(buffer);
    }
  }
  return buffers;
}

/** Detach abandoned copies/results so large stale buffers become immediately reclaimable. */
function releaseBuffers(buffers: readonly ArrayBuffer[]): void {
  const transferable = buffers.filter((buffer) => buffer.byteLength > 0);
  if (transferable.length === 0) return;
  if (typeof MessageChannel !== 'undefined') {
    const channel = new MessageChannel();
    try { channel.port1.postMessage(null, [...transferable]); }
    catch { /* A buffer may already have been transferred by a worker. */ }
    finally { channel.port1.close(); channel.port2.close(); }
    return;
  }
  if (typeof structuredClone === 'function') {
    try { structuredClone(null, { transfer: transferable }); }
    catch { /* Releasing stale buffers is best-effort on older WebViews. */ }
  }
}

function responseBuffers(response: ScanEncodeWorkerResponse): ArrayBuffer[] {
  return response.type === 'result' ? [response.artifact] : [];
}

function countersForSyncFallback(
  metrics: Omit<ScanEncodeCounters, 'bytesCopied'>,
): ScanEncodeCounters {
  return { ...metrics, bytesCopied: 0 };
}

export function releaseScanEncodeResult(result: ScanEncodeResult): void {
  releaseBuffers([result.bytes.buffer as ArrayBuffer]);
}

export class ScanEncodeClient {
  private nextRequestId = 1;
  private generation = 0;
  private pending: PendingScanEncode | null = null;
  private disposed = false;
  private readonly workerFactory: WorkerFactory;
  private readonly workerAvailable: boolean;

  constructor(workerFactory?: WorkerFactory) {
    this.workerFactory = workerFactory ?? (() => new ScanEncodeWorker());
    this.workerAvailable = workerFactory !== undefined || typeof Worker !== 'undefined';
  }

  get busy(): boolean { return this.pending !== null; }

  encode(snapshot: ScanPointCloudSnapshot, options: ScanEncodeClientOptions = {}): Promise<ScanEncodeResult> {
    if (this.disposed) return Promise.reject(new ScanEncodeCancelledError('Scan encode client disposed.'));
    this.cancel('A newer scan finish superseded this request.');
    const token = ++this.generation;
    const requestId = this.nextRequestId++;
    const generation = options.generation ?? token;
    const budget = {
      estimatedBytes: scanArtifactSnapshotArrayBytes(snapshot),
      limitBytes: options.budgetLimitBytes ?? SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES,
    };
    if (budget.estimatedBytes > budget.limitBytes) {
      return Promise.reject(new ScanEncodeBudgetExceededError(budget));
    }

    if (!this.workerAvailable) {
      try { options.onFallback?.(); }
      catch { /* Diagnostics must not affect the synchronous compatibility path. */ }
      try {
        const encoded = encodeScanArtifactWithIdentityAndMetrics(snapshot);
        return Promise.resolve({
          bytes: encoded.bytes,
          identity: encoded.identity,
          counters: countersForSyncFallback(encoded.metrics),
        });
      } catch (reason) {
        return Promise.reject(reason);
      }
    }

    let copies: ScanPointCloudSnapshot | null = null;
    let worker: Worker;
    try {
      copies = copySnapshot(snapshot);
      worker = this.workerFactory();
      return new Promise<ScanEncodeResult>((resolve, reject) => {
        const pending: PendingScanEncode = { requestId, generation, token, worker, copies: copies!, resolve, reject };
        this.pending = pending;
        worker.addEventListener('message', this.handleMessage);
        worker.addEventListener('error', this.handleError);
        worker.addEventListener('messageerror', this.handleMessageError);
        const copyBytes = scanArtifactSnapshotArrayBytes(copies!);
        const request: ScanEncodeWorkerRequest = {
          type: 'encode', requestId, generation, snapshot: copies!, copyBytes,
        };
        try { worker.postMessage(request, snapshotBuffers(copies!)); }
        catch (reason) { this.finishWithError(pending, reason); }
      });
    } catch (reason) {
      if (copies !== null) releaseBuffers(snapshotBuffers(copies));
      return Promise.reject(reason);
    }
  }

  cancel(reason = 'Scan encoding cancelled.'): void {
    this.generation += 1;
    const pending = this.pending;
    if (pending === null) return;
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    if (pending.copies !== null) releaseBuffers(snapshotBuffers(pending.copies));
    pending.copies = null;
    pending.reject(new ScanEncodeCancelledError(reason));
  }

  dispose(): void {
    if (this.disposed) return;
    this.cancel('Scan encode client disposed.');
    this.disposed = true;
  }

  private detachWorker(worker: Worker): void {
    worker.removeEventListener('message', this.handleMessage);
    worker.removeEventListener('error', this.handleError);
    worker.removeEventListener('messageerror', this.handleMessageError);
  }

  private finishWithError(pending: PendingScanEncode, reason: unknown): void {
    if (this.pending !== pending) return;
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    if (pending.copies !== null) releaseBuffers(snapshotBuffers(pending.copies));
    pending.copies = null;
    pending.reject(reason instanceof Error ? reason : new Error(String(reason)));
  }

  private readonly handleMessage = (event: MessageEvent<ScanEncodeWorkerResponse>): void => {
    const response = event.data;
    const pending = this.pending;
    if (pending === null || response.requestId !== pending.requestId || response.generation !== pending.generation || pending.token !== this.generation) {
      releaseBuffers(responseBuffers(response));
      return;
    }
    if (response.type === 'error') {
      this.finishWithError(pending, new Error(response.error));
      return;
    }
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    pending.copies = null;
    pending.resolve({ bytes: new Uint8Array(response.artifact), identity: response.identity, counters: response.counters });
  };

  private readonly handleError = (event: ErrorEvent): void => {
    const pending = this.pending;
    if (pending !== null) this.finishWithError(pending, new Error(event.message || 'Scan encode worker failed.'));
  };

  private readonly handleMessageError = (): void => {
    const pending = this.pending;
    if (pending !== null) this.finishWithError(pending, new Error('Scan encode worker sent an unreadable response.'));
  };
}
