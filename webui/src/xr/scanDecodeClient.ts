import ScanDecodeWorker from './scanDecodeWorker?worker&inline';
import {
  decodeScanArtifactWithIdentityAndMetrics,
  MAX_SCAN_ARTIFACT_BYTES,
} from './scanArtifact';
import type { ScanIdentity } from './scanIdentity';
import type { ScanPointCloudSnapshot } from './webxrRuntime';
import type {
  ScanDecodeCounters,
  ScanDecodeWorkerRequest,
  ScanDecodeWorkerResponse,
} from './scanDecodeWorker';

/** Worst-case source plus payload arrays and legacy normalConfidence expansion. */
export const SCAN_DECODE_TRANSIENT_BUDGET_BYTES = MAX_SCAN_ARTIFACT_BYTES * 5;

export interface ScanDecodeBudgetEstimate {
  estimatedBytes: number;
  limitBytes: number;
}

export interface ScanDecodeClientOptions {
  /** Owner generation supplied by App; stale owner responses are never accepted. */
  generation?: number;
  budgetLimitBytes?: number;
  /** Transfer a whole source buffer when its caller no longer needs the bytes. */
  transferInput?: boolean;
  onFallback?: () => void;
}

export interface ScanDecodeResult {
  snapshot: ScanPointCloudSnapshot;
  identity: ScanIdentity;
  counters: ScanDecodeCounters;
}

export class ScanDecodeBudgetExceededError extends Error {
  readonly code = 'scan_decode_transient_budget_exceeded';

  constructor(readonly estimate: ScanDecodeBudgetEstimate) {
    super(`scan_decode_transient_budget_exceeded: ${estimate.estimatedBytes} bytes estimated, ${estimate.limitBytes} byte limit`);
    this.name = 'ScanDecodeBudgetExceededError';
  }
}

export class ScanDecodeCancelledError extends Error {
  readonly code = 'scan_decode_cancelled';

  constructor(message = 'Scan decoding cancelled.') {
    super(message);
    this.name = 'ScanDecodeCancelledError';
  }
}

export class ScanDecodeWorkerError extends Error {
  readonly code = 'scan_decode_worker_failed';

  constructor(message = 'Scan decode worker failed.') {
    super(message);
    this.name = 'ScanDecodeWorkerError';
  }
}

interface PendingScanDecode {
  requestId: number;
  generation: number;
  token: number;
  worker: Worker;
  inputBuffer: ArrayBuffer | null;
  resolve: (result: ScanDecodeResult) => void;
  reject: (reason: unknown) => void;
}

type WorkerFactory = () => Worker;

function sourceView(source: ArrayBuffer | Uint8Array): Uint8Array {
  return source instanceof Uint8Array ? source : new Uint8Array(source);
}

/** Estimate worker input plus output arrays without parsing or validating metadata on the UI thread. */
export function estimateScanDecodeTransientBytes(source: ArrayBuffer | Uint8Array): number {
  const bytes = sourceView(source);
  let outputArrayBytes = 0;
  if (bytes.byteLength >= 20) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const headerLength = view.getUint32(8, true);
    const payloadLength = view.getUint32(12, true);
    if (headerLength > 0 && 20 + headerLength + payloadLength === bytes.byteLength) {
      /* A legacy normalConfidence value uses at most four output bytes per header byte. */
      outputArrayBytes = payloadLength + headerLength * Float32Array.BYTES_PER_ELEMENT;
    }
  }
  return bytes.byteLength + outputArrayBytes;
}

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

function resultBuffers(result: ScanDecodeResult): ArrayBuffer[] {
  const buffers = new Set<ArrayBuffer>();
  for (const value of Object.values(result.snapshot)) {
    if ((value instanceof Float32Array || value instanceof Uint8Array) && value.buffer instanceof ArrayBuffer) {
      buffers.add(value.buffer);
    }
  }
  return [...buffers];
}

function responseBuffers(response: ScanDecodeWorkerResponse | null): ArrayBuffer[] {
  if (response?.type !== 'result') return [];
  return resultBuffers(response);
}

function prepareWorkerInput(
  source: ArrayBuffer | Uint8Array,
  transferInput: boolean,
): { buffer: ArrayBuffer; bytesCopied: number } {
  if (transferInput && source instanceof ArrayBuffer) return { buffer: source, bytesCopied: 0 };
  if (transferInput && source instanceof Uint8Array && source.buffer instanceof ArrayBuffer &&
      source.byteOffset === 0 && source.byteLength === source.buffer.byteLength) {
    return { buffer: source.buffer, bytesCopied: 0 };
  }
  const copy = new Uint8Array(sourceView(source).byteLength);
  copy.set(sourceView(source));
  return { buffer: copy.buffer, bytesCopied: copy.byteLength };
}

function workerResultIsValid(response: ScanDecodeWorkerResponse): response is Extract<ScanDecodeWorkerResponse, { type: 'result' }> {
  if (response.type !== 'result' || response.snapshot === null || typeof response.snapshot !== 'object') return false;
  if (response.identity === null || typeof response.identity !== 'object' ||
      typeof response.identity.byteLength !== 'number' || typeof response.identity.crc32 !== 'number' ||
      typeof response.identity.frameId !== 'string') return false;
  return response.counters !== null && typeof response.counters === 'object';
}

export function releaseScanDecodeResult(result: ScanDecodeResult): void {
  releaseBuffers(resultBuffers(result));
}

export class ScanDecodeClient {
  private nextRequestId = 1;
  private generation = 0;
  private pending: PendingScanDecode | null = null;
  private disposed = false;
  private readonly workerFactory: WorkerFactory;
  private readonly workerAvailable: boolean;

  constructor(workerFactory?: WorkerFactory) {
    this.workerFactory = workerFactory ?? (() => new ScanDecodeWorker());
    this.workerAvailable = workerFactory !== undefined || typeof Worker !== 'undefined';
  }

  get busy(): boolean { return this.pending !== null; }

  decode(source: ArrayBuffer | Uint8Array, options: ScanDecodeClientOptions = {}): Promise<ScanDecodeResult> {
    if (this.disposed) return Promise.reject(new ScanDecodeCancelledError('Scan decode client disposed.'));
    this.cancel('A newer saved scan load superseded this request.');
    const token = ++this.generation;
    const requestId = this.nextRequestId++;
    const generation = options.generation ?? token;
    const budget = {
      estimatedBytes: estimateScanDecodeTransientBytes(source),
      limitBytes: options.budgetLimitBytes ?? SCAN_DECODE_TRANSIENT_BUDGET_BYTES,
    };
    if (budget.estimatedBytes > budget.limitBytes) {
      return Promise.reject(new ScanDecodeBudgetExceededError(budget));
    }

    if (!this.workerAvailable) {
      try { options.onFallback?.(); }
      catch { /* Diagnostics must not affect the synchronous compatibility path. */ }
      try {
        const decoded = decodeScanArtifactWithIdentityAndMetrics(source);
        return Promise.resolve({ snapshot: decoded.snapshot, identity: decoded.identity, counters: { ...decoded.metrics, bytesCopied: 0 } });
      } catch (reason) {
        return Promise.reject(reason);
      }
    }

    let worker: Worker;
    let input: { buffer: ArrayBuffer; bytesCopied: number } | null = null;
    try {
      worker = this.workerFactory();
      input = prepareWorkerInput(source, options.transferInput ?? false);
      return new Promise<ScanDecodeResult>((resolve, reject) => {
        const pending: PendingScanDecode = {
          requestId, generation, token, worker, inputBuffer: input!.buffer, resolve, reject,
        };
        this.pending = pending;
        worker.addEventListener('message', this.handleMessage);
        worker.addEventListener('error', this.handleError);
        worker.addEventListener('messageerror', this.handleMessageError);
        const request: ScanDecodeWorkerRequest = {
          type: 'decode', requestId, generation, artifact: input!.buffer, bytesCopied: input!.bytesCopied,
        };
        try { worker.postMessage(request, [input!.buffer]); }
        catch (reason) { this.finishWithError(pending, reason); }
      });
    } catch (reason) {
      if (input !== null) releaseBuffers([input.buffer]);
      return Promise.reject(reason);
    }
  }

  cancel(reason = 'Scan decoding cancelled.'): void {
    this.generation += 1;
    const pending = this.pending;
    if (pending === null) return;
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    if (pending.inputBuffer !== null) releaseBuffers([pending.inputBuffer]);
    pending.inputBuffer = null;
    pending.reject(new ScanDecodeCancelledError(reason));
  }

  dispose(): void {
    if (this.disposed) return;
    this.cancel('Scan decode client disposed.');
    this.disposed = true;
  }

  private detachWorker(worker: Worker): void {
    worker.removeEventListener('message', this.handleMessage);
    worker.removeEventListener('error', this.handleError);
    worker.removeEventListener('messageerror', this.handleMessageError);
  }

  private finishWithError(pending: PendingScanDecode, reason: unknown): void {
    if (this.pending !== pending) return;
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    if (pending.inputBuffer !== null) releaseBuffers([pending.inputBuffer]);
    pending.inputBuffer = null;
    pending.reject(reason instanceof Error ? reason : new ScanDecodeWorkerError(String(reason)));
  }

  private readonly handleMessage = (event: MessageEvent<ScanDecodeWorkerResponse>): void => {
    const response = event.data ?? null;
    const pending = this.pending;
    if (pending === null || response === null || response.requestId !== pending.requestId ||
        response.generation !== pending.generation || pending.token !== this.generation) {
      if (response !== null) releaseBuffers(responseBuffers(response));
      return;
    }
    if (response.type === 'error') {
      this.finishWithError(pending, new Error(response.error));
      return;
    }
    if (!workerResultIsValid(response)) {
      this.finishWithError(pending, new ScanDecodeWorkerError('Scan decode worker sent an invalid response.'));
      return;
    }
    this.pending = null;
    this.detachWorker(pending.worker);
    pending.worker.terminate();
    pending.inputBuffer = null;
    pending.resolve({ snapshot: response.snapshot, identity: response.identity, counters: response.counters });
  };

  private readonly handleError = (event: ErrorEvent): void => {
    const pending = this.pending;
    if (pending !== null) this.finishWithError(pending, new ScanDecodeWorkerError(event.message || 'Scan decode worker failed.'));
  };

  private readonly handleMessageError = (): void => {
    const pending = this.pending;
    if (pending !== null) this.finishWithError(pending, new ScanDecodeWorkerError('Scan decode worker sent an unreadable response.'));
  };
}
