import LiveMeshWorker from './liveMeshWorker?worker&inline';
import type { LiveMeshInput, LiveMeshResult } from './liveMeshCore';
import type { LiveMeshRequest, LiveMeshResponse } from './liveMeshWorker';

export interface LiveMeshWorkerLike {
  postMessage(message: LiveMeshRequest, transfer?: Transferable[]): void;
  terminate(): void;
  onmessage: ((event: { data: LiveMeshResponse }) => void) | null;
  onerror: ((event: unknown) => void) | null;
}

export const LIVE_MESH_MIN_INTERVAL_MS = 1800;
/** After this many failed builds in a row the live mesh switches itself off for the session: the scan matters more than the preview. */
export const LIVE_MESH_MAX_CONSECUTIVE_FAILURES = 3;

export interface LiveMeshClientOptions {
  createWorker?: () => LiveMeshWorkerLike;
  now?: () => number;
  minIntervalMs?: number;
  onResult: (result: LiveMeshResult, elapsedMs: number) => void;
  onDisabled?: (reason: string) => void;
}

/**
 * One build at a time, never faster than the interval, never queued: while a build runs (or the interval has not passed) a request is simply refused,
 * so a slow phone gets a slower preview instead of a growing backlog. The worker is created on the first accepted request.
 */
export class LiveMeshClient {
  private worker: LiveMeshWorkerLike | null = null;
  private inFlight: number | null = null;
  private nextId = 1;
  private lastAcceptedMs = Number.NEGATIVE_INFINITY;
  private failures = 0;
  private disposed = false;
  private disabled = false;
  private readonly createWorker: () => LiveMeshWorkerLike;
  private readonly now: () => number;
  private readonly minIntervalMs: number;

  constructor(private readonly options: LiveMeshClientOptions) {
    this.createWorker = options.createWorker ?? (() => new LiveMeshWorker() as unknown as LiveMeshWorkerLike);
    this.now = options.now ?? (() => performance.now());
    this.minIntervalMs = options.minIntervalMs ?? LIVE_MESH_MIN_INTERVAL_MS;
  }

  get busy(): boolean { return this.inFlight !== null; }
  get isDisabled(): boolean { return this.disabled; }

  /** Returns true when the request was accepted. `build` is only called then, so the caller does not gather points for nothing. */
  request(build: () => LiveMeshInput | null, minIntervalMs: number = this.minIntervalMs): boolean {
    if (this.disposed || this.disabled || this.inFlight !== null) return false;
    const time = this.now();
    if (time - this.lastAcceptedMs < minIntervalMs) return false;
    const input = build();
    if (input === null) return false;
    this.lastAcceptedMs = time;
    try {
      if (this.worker === null) {
        this.worker = this.createWorker();
        this.worker.onmessage = (event) => this.handle(event.data);
        this.worker.onerror = () => this.fail('worker error');
      }
      const requestId = this.nextId++;
      this.inFlight = requestId;
      this.worker.postMessage({ type: 'mesh', requestId, input }, [input.positions.buffer, input.normals.buffer, input.confidence.buffer]);
      return true;
    } catch (error) {
      this.inFlight = null;
      this.fail(error instanceof Error ? error.message : String(error));
      return false;
    }
  }

  private handle(message: LiveMeshResponse): void {
    if (this.disposed || message.requestId !== this.inFlight) return;
    this.inFlight = null;
    if (message.type === 'error') { this.fail(message.message); return; }
    this.failures = 0;
    this.options.onResult(message.result, message.elapsedMs);
  }

  private fail(reason: string): void {
    this.inFlight = null;
    this.failures += 1;
    if (this.failures >= LIVE_MESH_MAX_CONSECUTIVE_FAILURES) {
      this.disabled = true;
      this.options.onDisabled?.(reason);
    }
  }

  dispose(): void {
    this.disposed = true;
    this.inFlight = null;
    try { this.worker?.terminate(); } catch { /* already gone */ }
    this.worker = null;
  }
}
