import type { CpuDepthInformationLike, DepthFusionFrameContext } from './depthPointCloudV2';
import type { WebXrDepthDataFormat } from './cpuDepthSampler';
import { DepthFusionCore, createDepthFusionHandler } from './depthFusionCore';
import { DepthBufferPool, captureDepthFrameBuffer, type DepthFrameGeometry } from './depthFusionFrame';
import type {
  FusionFrameMessage, FusionGuidancePacket, FusionRequestKind, FusionRequestMessage, FusionStats, FusionWorkerRequest, FusionWorkerResponse,
} from './depthFusionProtocol';

/**
 * Main-thread side of the depth-fusion worker (Phase 2): bounded queue, back-pressure, buffer pool, generation ownership, bounded waits.
 *
 * Queue policy (the approved one):
 *  - at most `maxInFlight` frames are with the worker; an OPTIONAL frame (polish) beyond that is dropped and counted;
 *  - an evidence KEYFRAME may use `mandatoryReserve` more slots; only when even those are used is a keyframe dropped (counted, and `overloaded` is raised so
 *    that the sampling interval can be widened) - a keyframe is never dropped while there is a slot for it;
 *  - the depth buffers come from a pool of `maxInFlight + mandatoryReserve` buffers, so memory is bounded by construction;
 *  - Finish: `drain()` stops intake, waits (bounded) for every accepted frame and then the snapshot is requested; a worker that does not answer in time is
 *    reported, never waited on forever.
 * A worker that cannot be started falls back to running the SAME core inline on the main thread (CPU fallback). A worker that dies mid-scan is an error
 * the owner must surface: its accumulated state is gone, and silently starting an empty one would lose the scan unnoticed.
 */
export interface DepthFusionClientOptions {
  /** Returns a started worker, or null/throws when none can be had (then the inline fallback is used). */
  workerFactory?: () => Worker | null;
  maxInFlight?: number;
  mandatoryReserve?: number;
  requestTimeoutMs?: number;
  drainTimeoutMs?: number;
  clock?: () => number;
  onFailure?: (reason: string) => void;
  /** Called on the main thread whenever the worker finished a frame (changed points, the new statistics). */
  onFrameDone?: (changedPoints: number, stats: FusionStats) => void;
}

export const FUSION_DEFAULT_MAX_IN_FLIGHT = 3;
export const FUSION_DEFAULT_MANDATORY_RESERVE = 5;

export interface FusionFrameInput {
  depth: CpuDepthInformationLike;
  dataFormat: WebXrDepthDataFormat;
  geometry: DepthFrameGeometry;
  context: DepthFusionFrameContext;
  guidance: FusionGuidancePacket | null;
  mandatory: boolean;
}

export type SubmitOutcome = 'accepted' | 'dropped-optional' | 'dropped-keyframe' | 'unavailable';

export interface FusionCounters {
  submitted: number;
  accepted: number;
  droppedOptional: number;
  droppedKeyframes: number;
  maxInFlight: number;
  maxQueueDepth: number;
  workerMsTotal: number;
  frameRoundTripMs: number[];
}

interface Pending { resolve: (value: unknown) => void; reject: (reason: Error) => void; timer: ReturnType<typeof setTimeout> | null; kind: FusionRequestKind }

const ZERO_STATS: FusionStats = { pointCount: 0, promotedCount: 0, polishFrameCount: 0, keyframeCount: 0, integratedFrames: 0 };

export class DepthFusionClient {
  readonly mode: 'worker' | 'inline';
  private worker: Worker | null = null;
  private inline: { handle: (message: FusionWorkerRequest) => void; core: DepthFusionCore } | null = null;
  private readonly pool: DepthBufferPool;
  private readonly maxInFlight: number;
  private readonly mandatoryReserve: number;
  private readonly requestTimeoutMs: number;
  private readonly drainTimeoutMs: number;
  private readonly clock: () => number;
  private readonly onFailure: ((reason: string) => void) | undefined;
  private readonly onFrameDone: ((changedPoints: number, stats: FusionStats) => void) | undefined;
  private generation = 0;
  private nextSeq = 1;
  private nextRequestId = 1;
  private inFlight = new Map<number, { mandatory: boolean; sentAt: number; byteLength: number }>();
  private pending = new Map<number, Pending>();
  private drainWaiters: Array<() => void> = [];
  private intakeOpen = true;
  private failed: string | null = null;
  private disposed = false;
  private mirror: FusionStats = ZERO_STATS;
  readonly counters: FusionCounters = { submitted: 0, accepted: 0, droppedOptional: 0, droppedKeyframes: 0, maxInFlight: 0, maxQueueDepth: 0, workerMsTotal: 0, frameRoundTripMs: [] };

  constructor(options: DepthFusionClientOptions = {}) {
    this.maxInFlight = options.maxInFlight ?? FUSION_DEFAULT_MAX_IN_FLIGHT;
    this.mandatoryReserve = options.mandatoryReserve ?? FUSION_DEFAULT_MANDATORY_RESERVE;
    this.requestTimeoutMs = options.requestTimeoutMs ?? 10_000;
    this.drainTimeoutMs = options.drainTimeoutMs ?? 5_000;
    this.clock = options.clock ?? (() => performance.now());
    this.onFailure = options.onFailure;
    this.onFrameDone = options.onFrameDone;
    this.pool = new DepthBufferPool(this.maxInFlight + this.mandatoryReserve);
    let worker: Worker | null = null;
    try { worker = options.workerFactory?.() ?? null; } catch { worker = null; }
    if (worker !== null) {
      this.mode = 'worker';
      this.worker = worker;
      worker.addEventListener('message', this.handleMessage);
      worker.addEventListener('error', this.handleError);
      worker.addEventListener('messageerror', this.handleMessageError);
    } else {
      this.mode = 'inline';
      this.inline = createDepthFusionHandler((message) => this.deliver(message));
    }
  }

  get stats(): FusionStats { return this.mirror; }
  get currentGeneration(): number { return this.generation; }
  get queueDepth(): number { return this.inFlight.size; }
  get hasFailed(): string | null { return this.failed; }
  /** True while keyframes are being dropped or the queue is full: the owner should widen its depth sampling interval. */
  get overloaded(): boolean { return this.inFlight.size >= this.maxInFlight; }
  get inlineCore(): DepthFusionCore | null { return this.inline?.core ?? null; }

  /** Starts a new scan session: everything of the previous one is void. @returns the new generation */
  reset(): number {
    this.failPending('superseded by a new scan');
    this.generation += 1;
    this.intakeOpen = true;
    for (const { byteLength } of this.inFlight.values()) this.pool.release(null, byteLength);
    this.inFlight.clear();
    this.flushDrainWaiters();
    this.mirror = ZERO_STATS;
    this.post({ type: 'reset', generation: this.generation });
    return this.generation;
  }

  /** Whether a frame of that kind would be taken now (checked BEFORE the owner's keyframe selector changes its own state). */
  canAccept(mandatory: boolean): boolean {
    if (this.disposed || this.failed !== null || !this.intakeOpen) return false;
    return this.inFlight.size < (mandatory ? this.maxInFlight + this.mandatoryReserve : this.maxInFlight);
  }

  submit(input: FusionFrameInput): SubmitOutcome {
    if (this.disposed || this.failed !== null || !this.intakeOpen) return 'unavailable';
    this.counters.submitted += 1;
    const slots = input.mandatory ? this.maxInFlight + this.mandatoryReserve : this.maxInFlight;
    if (this.inFlight.size >= slots) {
      if (input.mandatory) this.counters.droppedKeyframes += 1; else this.counters.droppedOptional += 1;
      return input.mandatory ? 'dropped-keyframe' : 'dropped-optional';
    }
    const frame = captureDepthFrameBuffer(input.depth, input.dataFormat, this.pool);
    if (frame === null) {
      if (input.mandatory) this.counters.droppedKeyframes += 1; else this.counters.droppedOptional += 1;
      return input.mandatory ? 'dropped-keyframe' : 'dropped-optional';
    }
    const seq = this.nextSeq++;
    const message: FusionFrameMessage = { type: 'frame', generation: this.generation, seq, mandatory: input.mandatory, frame, geometry: input.geometry, context: input.context, guidance: input.guidance };
    this.inFlight.set(seq, { mandatory: input.mandatory, sentAt: this.clock(), byteLength: frame.data.byteLength });
    this.counters.accepted += 1;
    this.counters.maxInFlight = Math.max(this.counters.maxInFlight, this.inFlight.size);
    this.counters.maxQueueDepth = Math.max(this.counters.maxQueueDepth, this.inFlight.size);
    this.post(message, [frame.data]);
    return 'accepted';
  }

  request<T = unknown>(kind: FusionRequestKind, params?: FusionRequestMessage['params']): Promise<T> {
    if (this.disposed) return Promise.reject(new Error('depth fusion client disposed'));
    if (this.failed !== null) return Promise.reject(new Error(`depth fusion failed: ${this.failed}`));
    const id = this.nextRequestId++;
    return new Promise<T>((resolve, reject) => {
      const timer = this.requestTimeoutMs > 0 && this.mode === 'worker'
        ? setTimeout(() => { this.pending.delete(id); reject(new Error(`depth fusion ${kind} request timed out`)); }, this.requestTimeoutMs)
        : null;
      this.pending.set(id, { resolve: resolve as (value: unknown) => void, reject, timer, kind });
      this.post({ type: 'request', generation: this.generation, id, kind, ...(params === undefined ? {} : { params }) });
    });
  }

  /** Finish: no new frames; resolves when every accepted frame is done, or after the bounded wait with what is still outstanding. */
  drain(timeoutMs = this.drainTimeoutMs): Promise<{ drained: boolean; outstandingFrames: number; outstandingKeyframes: number }> {
    this.intakeOpen = false;
    const outcome = (drained: boolean) => ({ drained, outstandingFrames: this.inFlight.size, outstandingKeyframes: [...this.inFlight.values()].filter((f) => f.mandatory).length });
    if (this.inFlight.size === 0) return Promise.resolve(outcome(true));
    return new Promise((resolve) => {
      const timer = setTimeout(() => { this.drainWaiters = this.drainWaiters.filter((w) => w !== waiter); resolve(outcome(false)); }, timeoutMs);
      const waiter = (): void => { clearTimeout(timer); resolve(outcome(true)); };
      this.drainWaiters.push(waiter);
    });
  }

  /** Reopens intake after a drain that was not followed by a reset (a cancelled finish). */
  resumeIntake(): void { if (!this.disposed && this.failed === null) this.intakeOpen = true; }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.failPending('depth fusion client disposed');
    if (this.worker !== null) {
      this.worker.removeEventListener('message', this.handleMessage);
      this.worker.removeEventListener('error', this.handleError);
      this.worker.removeEventListener('messageerror', this.handleMessageError);
      this.worker.terminate();
      this.worker = null;
    }
    this.pool.clear();
    this.inFlight.clear();
    this.flushDrainWaiters();
  }

  private post(message: FusionWorkerRequest, transfer: ArrayBuffer[] = []): void {
    if (this.worker !== null) { try { this.worker.postMessage(message, transfer); } catch (error) { this.fail(error instanceof Error ? error.message : String(error)); } return; }
    this.inline?.handle(message);
  }

  private fail(reason: string): void {
    if (this.failed !== null) return;
    this.failed = reason;
    this.failPending(`depth fusion failed: ${reason}`);
    for (const { byteLength } of this.inFlight.values()) this.pool.release(null, byteLength);
    this.inFlight.clear();
    this.flushDrainWaiters();
    if (this.worker !== null) { this.worker.terminate(); this.worker = null; }
    this.onFailure?.(reason);
  }

  private failPending(reason: string): void {
    for (const pending of this.pending.values()) { if (pending.timer !== null) clearTimeout(pending.timer); pending.reject(new Error(reason)); }
    this.pending.clear();
  }

  private flushDrainWaiters(): void {
    const waiters = this.drainWaiters; this.drainWaiters = [];
    for (const waiter of waiters) waiter();
  }

  private deliver(message: FusionWorkerResponse): void {
    if (this.disposed || message.generation !== this.generation) {
      if (message.type === 'frameDone' && message.buffer !== null) this.pool.release(message.buffer, message.buffer.byteLength);
      return;                                                                                    /* a late answer of an older scan changes nothing */
    }
    if (message.type === 'frameDone') {
      const entry = this.inFlight.get(message.seq);
      if (entry === undefined) { if (message.buffer !== null) this.pool.release(message.buffer, message.buffer.byteLength); return; }
      this.inFlight.delete(message.seq);
      this.pool.release(message.buffer, entry.byteLength);
      this.mirror = message.stats;
      this.counters.workerMsTotal += message.workMs;
      this.counters.frameRoundTripMs.push(this.clock() - entry.sentAt);
      if (this.counters.frameRoundTripMs.length > 512) this.counters.frameRoundTripMs.shift();
      this.onFrameDone?.(message.changedPoints, message.stats);
      if (this.inFlight.size === 0) this.flushDrainWaiters();
      return;
    }
    if (message.type === 'response') {
      const pending = this.pending.get(message.id);
      if (pending === undefined) return;
      this.pending.delete(message.id);
      if (pending.timer !== null) clearTimeout(pending.timer);
      this.mirror = message.stats;
      pending.resolve(message.payload);
      return;
    }
    if (message.seq !== undefined) {
      const entry = this.inFlight.get(message.seq);
      if (entry !== undefined) { this.inFlight.delete(message.seq); this.pool.release(null, entry.byteLength); if (this.inFlight.size === 0) this.flushDrainWaiters(); }
      this.counters.droppedOptional += entry?.mandatory === false ? 1 : 0;
      this.counters.droppedKeyframes += entry?.mandatory === true ? 1 : 0;
      return;
    }
    if (message.id !== undefined) {
      const pending = this.pending.get(message.id);
      if (pending !== undefined) { this.pending.delete(message.id); if (pending.timer !== null) clearTimeout(pending.timer); pending.reject(new Error(message.message)); }
    }
  }

  private readonly handleMessage = (event: MessageEvent<FusionWorkerResponse>): void => { this.deliver(event.data); };
  private readonly handleError = (event: ErrorEvent): void => { this.fail(event.message || 'worker error'); };
  private readonly handleMessageError = (): void => { this.fail('unreadable worker message'); };
}
