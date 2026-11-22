import ContinuationWorker from './continuationWorker?worker&inline';
import {
  estimateContinuationBudget,
  type ContinuationBudgetEstimate,
  type ScanContinuationPhase,
  type ScanContinuationResult,
} from './scanContinuation';
import type { ScanPointCloudSnapshot } from './webxrRuntime';
import type { ContinuationWorkerRequest, ContinuationWorkerResponse } from './continuationWorker';

export interface ContinuationClientOptions {
  /** Owner generation supplied by App; stale owner responses are never accepted. */
  generation?: number;
  /** Lowered to the device post-scan budget when the capability profile is known. */
  budgetLimitBytes?: number;
  onPhase?: (phase: ScanContinuationPhase) => void;
  /** The B -> A transform found from known tags (issue #45): used as the registration. */
  seed?: { yaw: number; tx: number; ty: number; tz: number } | null;
}

export class ContinuationBudgetExceededError extends Error {
  readonly code = 'continuation_transient_budget_exceeded';

  constructor(readonly estimate: ContinuationBudgetEstimate) {
    super(`continuation_transient_budget_exceeded: ${estimate.estimatedBytes} bytes estimated, ${estimate.limitBytes} byte limit`);
    this.name = 'ContinuationBudgetExceededError';
  }
}

export class ContinuationCancelledError extends Error {
  readonly code = 'continuation_cancelled';

  constructor(message = 'Scan continuation cancelled.') {
    super(message);
    this.name = 'ContinuationCancelledError';
  }
}

interface PendingContinuation {
  requestId: number;
  generation: number;
  token: number;
  worker: Worker;
  copies: ScanPointCloudSnapshot[];
  onPhase?: (phase: ScanContinuationPhase) => void;
  resolve: (result: ScanContinuationResult) => void;
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

function responseBuffers(response: ContinuationWorkerResponse): ArrayBuffer[] {
  if (response.type !== 'result' || !response.result.ok) return [];
  return snapshotBuffers(response.result.snapshot);
}

export class ContinuationClient {
  private nextRequestId = 1;
  private generation = 0;
  private pending: PendingContinuation | null = null;
  private disposed = false;
  private readonly workerFactory: WorkerFactory;

  constructor(workerFactory: WorkerFactory = () => new ContinuationWorker()) {
    this.workerFactory = workerFactory;
  }

  get busy(): boolean { return this.pending !== null; }

  continue(
    base: ScanPointCloudSnapshot,
    extension: ScanPointCloudSnapshot,
    options: ContinuationClientOptions = {},
  ): Promise<ScanContinuationResult> {
    if (this.disposed) return Promise.reject(new ContinuationCancelledError('Continuation client disposed.'));
    this.cancel('A newer continuation superseded this request.');
    const token = ++this.generation;
    const requestId = this.nextRequestId++;
    const generation = options.generation ?? token;
    const budget = estimateContinuationBudget(base, extension, options.budgetLimitBytes);
    if (budget.estimatedBytes > budget.limitBytes) {
      return Promise.reject(new ContinuationBudgetExceededError(budget));
    }

    let copies: ScanPointCloudSnapshot[] = [];
    let worker: Worker;
    try {
      const baseCopy = copySnapshot(base);
      copies.push(baseCopy);
      const extensionCopy = copySnapshot(extension);
      copies.push(extensionCopy);
      worker = this.workerFactory();
      return new Promise<ScanContinuationResult>((resolve, reject) => {
        const pending: PendingContinuation = {
          requestId, generation, token, worker, copies,
          ...(options.onPhase === undefined ? {} : { onPhase: options.onPhase }), resolve, reject,
        };
        this.pending = pending;
        worker.addEventListener('message', this.handleMessage);
        worker.addEventListener('error', this.handleError);
        worker.addEventListener('messageerror', this.handleMessageError);
        const transfer = [...snapshotBuffers(baseCopy), ...snapshotBuffers(extensionCopy)];
        const request: ContinuationWorkerRequest = {
          type: 'continue', requestId, generation, base: baseCopy, extension: extensionCopy, options: { budget, ...(options.seed === undefined || options.seed === null ? {} : { seed: options.seed }) },
        };
        try { worker.postMessage(request, transfer); }
        catch (reason) { this.finishWithError(pending, reason); }
      });
    } catch (reason) {
      releaseBuffers(copies.flatMap(snapshotBuffers));
      return Promise.reject(reason);
    }
  }

  cancel(reason = 'Scan continuation cancelled.'): void {
    this.generation += 1;
    const pending = this.pending;
    if (pending === null) return;
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    releaseBuffers(pending.copies.flatMap(snapshotBuffers));
    pending.copies = [];
    pending.reject(new ContinuationCancelledError(reason));
  }

  dispose(): void {
    if (this.disposed) return;
    this.cancel('Continuation client disposed.');
    this.disposed = true;
  }

  private detachWorker(worker: Worker): void {
    worker.removeEventListener('message', this.handleMessage);
    worker.removeEventListener('error', this.handleError);
    worker.removeEventListener('messageerror', this.handleMessageError);
  }

  private finishWithError(pending: PendingContinuation, reason: unknown): void {
    if (this.pending !== pending) return;
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    releaseBuffers(pending.copies.flatMap(snapshotBuffers));
    pending.copies = [];
    pending.reject(reason instanceof Error ? reason : new Error(String(reason)));
  }

  private readonly handleMessage = (event: MessageEvent<ContinuationWorkerResponse>): void => {
    const response = event.data;
    const pending = this.pending;
    if (pending === null || response.requestId !== pending.requestId || response.generation !== pending.generation || pending.token !== this.generation) {
      releaseBuffers(responseBuffers(response));
      return;
    }
    if (response.type === 'phase') {
      try { pending.onPhase?.(response.phase); }
      catch { /* Progress UI cannot affect the continuation result. */ }
      return;
    }
    if (response.type === 'error') {
      this.finishWithError(pending, new Error(response.error));
      return;
    }
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    pending.copies = [];
    pending.resolve(response.result);
  };

  private readonly handleError = (event: ErrorEvent): void => {
    const pending = this.pending;
    if (pending === null) return;
    this.finishWithError(pending, new Error(event.message || 'Continuation worker failed.'));
  };

  private readonly handleMessageError = (): void => {
    const pending = this.pending;
    if (pending !== null) this.finishWithError(pending, new Error('Continuation worker sent an unreadable response.'));
  };
}
