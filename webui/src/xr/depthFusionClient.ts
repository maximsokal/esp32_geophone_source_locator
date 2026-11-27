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
 * Admission is decided BEFORE capture (fusionCaptureGovernor); nothing accepted is ever discarded. Queue bounds:
 *  - at most `maxInFlight` frames are with the worker; an OPTIONAL frame (polish) beyond that is dropped and counted;
 *  - an evidence KEYFRAME may use `mandatoryReserve` more slots; only when even those are used is a keyframe dropped (counted, and `overloaded` is raised so
 *    that the sampling interval can be widened) - a keyframe is never dropped while there is a slot for it;
 *  - the depth buffers come from a pool of `maxInFlight + mandatoryReserve` buffers, so memory is bounded by construction;
 *  - Finish: `drain()` stops intake, waits (bounded) for every accepted frame and then the snapshot is requested; a worker that does not answer in time is
 *    reported, never waited on forever.
 * A worker that cannot be started falls back to running the SAME core inline on the main thread (CPU fallback).
 *
 * Recovery: every `checkpointEveryFrames` fused frames the worker sends its state to the keeper worker (never through the main thread), and the client keeps
 * a bounded JOURNAL of the accepted frames since the last stored checkpoint. When the worker dies, a replacement restores the checkpoint, fuses the journal
 * again and answers whatever was still pending. Frames that could not be kept (journal overflow) are counted and reported, never hidden; when no
 * replacement can be had, the failure is terminal and reported.
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
  /** Also told the worker's fusion time of every frame (drives the capture rate). */
  onFrameTiming?: (workMs: number, queueDepth: number) => void;
  /** Holds checkpoints; without one a crash can only be recovered while the journal still holds every frame. */
  keeperFactory?: () => Worker | null;
  channelFactory?: () => { port1: unknown; port2: unknown };
  checkpointEveryFrames?: number;
  journalLimit?: number;
  maxRestarts?: number;
  /** How long a replacement worker may wait for the keeper's checkpoint before it is replaced again without one. */
  restoreTimeoutMs?: number;
  onRecovered?: (report: FusionRecoveryReport) => void;
}

export interface FusionRecoveryReport {
  reason: string;
  /** Last frame contained in the restored checkpoint; -1 when there was none. */
  restoredThroughSeq: number;
  replayedFrames: number;
  /** Accepted frames that are in neither the checkpoint nor the journal: their evidence is gone and must be scanned again. */
  lostFrames: number;
  lostKeyframes: number;
}

export const FUSION_DEFAULT_CHECKPOINT_EVERY = 20;
export const FUSION_DEFAULT_MAX_RESTARTS = 2;

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
  /** Accepted, but their evidence did not reach the state (frame error, or gone in a crash). Must stay 0 in normal operation. */
  lostFrames: number;
  lostKeyframes: number;
  recoveries: number;
  replayedFrames: number;
  checkpointsStored: number;
  maxJournal: number;
  maxInFlight: number;
  maxQueueDepth: number;
  workerMsTotal: number;
  frameRoundTripMs: number[];
}

interface Pending { resolve: (value: unknown) => void; reject: (reason: Error) => void; timer: ReturnType<typeof setTimeout> | null; kind: FusionRequestKind; params: FusionRequestMessage['params'] }

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
  readonly counters: FusionCounters = { submitted: 0, accepted: 0, droppedOptional: 0, droppedKeyframes: 0, lostFrames: 0, lostKeyframes: 0, recoveries: 0, replayedFrames: 0, checkpointsStored: 0, maxJournal: 0, maxInFlight: 0, maxQueueDepth: 0, workerMsTotal: 0, frameRoundTripMs: [] };
  private readonly options: DepthFusionClientOptions;
  private keeper: Worker | null = null;
  private readonly journal = new Map<number, FusionFrameMessage>();
  /* seq -> keyframe, for EVERY accepted frame that has left the journal (covered by a stored checkpoint, or pushed out by the bound): if the checkpoint
     that covered it cannot be restored, it is lost like any other and must be counted. A number and a flag per frame. */
  private readonly evicted = new Map<number, boolean>();
  private storedThroughSeq = -1;
  private checkpointPending = false;
  private framesSinceCheckpoint = 0;
  private restarts = 0;
  private recovering: { reason: string; replayed: number } | null = null;
  private restoreTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(options: DepthFusionClientOptions = {}) {
    this.maxInFlight = options.maxInFlight ?? FUSION_DEFAULT_MAX_IN_FLIGHT;
    this.mandatoryReserve = options.mandatoryReserve ?? FUSION_DEFAULT_MANDATORY_RESERVE;
    this.requestTimeoutMs = options.requestTimeoutMs ?? 10_000;
    this.drainTimeoutMs = options.drainTimeoutMs ?? 5_000;
    this.clock = options.clock ?? (() => performance.now());
    this.onFailure = options.onFailure;
    this.onFrameDone = options.onFrameDone;
    this.options = options;
    this.pool = new DepthBufferPool(this.maxInFlight + this.mandatoryReserve);
    let worker: Worker | null = null;
    try { worker = options.workerFactory?.() ?? null; } catch { worker = null; }
    if (worker !== null) {
      this.mode = 'worker';
      this.worker = worker;
      this.listen(worker);
      let keeper: Worker | null = null;
      try { keeper = options.keeperFactory?.() ?? null; } catch { keeper = null; }
      if (keeper !== null) {
        this.keeper = keeper;
        keeper.addEventListener('message', this.handleKeeperMessage);
        keeper.addEventListener('error', this.handleKeeperError);
        this.attachKeeper();
      }
    } else {
      this.mode = 'inline';
      this.inline = createDepthFusionHandler((message) => this.deliver(message));
    }
  }

  get stats(): FusionStats { return this.mirror; }
  get limits(): { queueDepth: number; maxInFlight: number; mandatoryReserve: number } { return { queueDepth: this.inFlight.size, maxInFlight: this.maxInFlight, mandatoryReserve: this.mandatoryReserve }; }
  get journalSize(): number { return this.journal.size; }
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
    this.forgetRecoveryState();
    this.restarts = 0;
    this.keeper?.postMessage({ type: 'drop' });
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
    if (this.mode === 'worker') this.remember(message);
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
      this.pending.set(id, { resolve: resolve as (value: unknown) => void, reject, timer, kind, params });
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
    this.releaseWorker();
    this.dropKeeper();
    this.forgetRecoveryState();
    this.pool.clear();
    this.inFlight.clear();
    this.flushDrainWaiters();
  }

  /** Replaces a worker that stopped answering. @returns false when no replacement could take over (then the client has failed). */
  restart(reason: string): boolean {
    if (this.disposed || this.failed !== null) return false;
    if (this.recover(reason)) return true;
    this.fail(reason);
    return false;
  }

  private listen(worker: Worker): void {
    worker.addEventListener('message', this.handleMessage);
    worker.addEventListener('error', this.handleError);
    worker.addEventListener('messageerror', this.handleMessageError);
  }

  private releaseWorker(): void {
    if (this.worker === null) return;
    this.worker.removeEventListener('message', this.handleMessage);
    this.worker.removeEventListener('error', this.handleError);
    this.worker.removeEventListener('messageerror', this.handleMessageError);
    this.worker.terminate();
    this.worker = null;
  }

  private channel(): { port1: unknown; port2: unknown } { return this.options.channelFactory?.() ?? new MessageChannel(); }

  private attachKeeper(): void {
    if (this.keeper === null || this.worker === null) return;
    const channel = this.channel();
    this.keeper.postMessage({ type: 'listen', port: channel.port1 }, [channel.port1 as Transferable]);
    this.post({ type: 'attachKeeper', generation: this.generation, port: channel.port2 as never }, [channel.port2 as Transferable]);
  }

  private dropKeeper(): void {
    if (this.keeper === null) return;
    this.keeper.removeEventListener('message', this.handleKeeperMessage);
    this.keeper.removeEventListener('error', this.handleKeeperError);
    this.keeper.terminate();
    this.keeper = null;
    this.checkpointPending = false;
  }

  /** A keeper that fails takes the checkpoints with it; recovery then rests on the journal and reports what that cannot give back. */
  private readonly handleKeeperError = (): void => {
    const waiting = this.restoreTimer !== null;
    this.dropKeeper();
    if (waiting) this.restoreWithoutKeeper('checkpoint keeper failed during restore');
  };

  private clearRestoreTimer(): void { if (this.restoreTimer !== null) { clearTimeout(this.restoreTimer); this.restoreTimer = null; } }

  /** The replacement is still waiting for a checkpoint that will not come: replace it once more, from the journal alone (not counted as another crash). */
  private restoreWithoutKeeper(reason: string): void {
    this.clearRestoreTimer();
    this.dropKeeper();
    this.storedThroughSeq = -1;
    const original = this.recovering?.reason ?? reason;
    this.restarts -= 1;
    this.counters.recoveries -= 1;
    if (!this.recover(original)) this.fail(reason);
  }

  private forgetRecoveryState(): void {
    this.clearRestoreTimer();
    this.journal.clear(); this.evicted.clear();
    this.storedThroughSeq = -1; this.checkpointPending = false; this.framesSinceCheckpoint = 0; this.recovering = null;
  }

  /** Keeps a copy of an accepted frame until a stored checkpoint contains it (the original buffer is transferred away). */
  private remember(message: FusionFrameMessage): void {
    this.journal.set(message.seq, { ...message, frame: { ...message.frame, data: message.frame.data.slice(0) } });
    const limit = this.options.journalLimit ?? (this.options.checkpointEveryFrames ?? FUSION_DEFAULT_CHECKPOINT_EVERY) * 2 + this.maxInFlight + this.mandatoryReserve;
    while (this.journal.size > limit) {
      const oldest = this.journal.keys().next().value as number;
      this.evicted.set(oldest, this.journal.get(oldest)!.mandatory);
      this.journal.delete(oldest);
    }
    this.counters.maxJournal = Math.max(this.counters.maxJournal, this.journal.size);
  }

  private recover(reason: string): boolean {
    if (this.mode !== 'worker' || this.restarts >= (this.options.maxRestarts ?? FUSION_DEFAULT_MAX_RESTARTS)) return false;
    this.releaseWorker();
    let replacement: Worker | null = null;
    try { replacement = this.options.workerFactory?.() ?? null; } catch { replacement = null; }
    if (replacement === null) return false;
    this.restarts += 1;
    this.counters.recoveries += 1;
    this.worker = replacement;
    this.listen(replacement);
    const frames = [...this.journal.values()].sort((a, b) => a.seq - b.seq);
    this.recovering = { reason, replayed: frames.length };
    this.checkpointPending = false; this.framesSinceCheckpoint = 0;
    this.post({ type: 'reset', generation: this.generation });
    if (this.keeper !== null) {
      const channel = this.channel();
      this.keeper.postMessage({ type: 'send', generation: this.generation, port: channel.port1 }, [channel.port1 as Transferable]);
      this.post({ type: 'restore', generation: this.generation, port: channel.port2 as never }, [channel.port2 as Transferable]);
      this.attachKeeper();
      this.clearRestoreTimer();
      this.restoreTimer = setTimeout(() => this.restoreWithoutKeeper('the checkpoint keeper did not answer'), this.options.restoreTimeoutMs ?? 3_000);
    }
    this.inFlight.clear();
    this.pool.clear();
    for (const frame of frames) {
      const copy: FusionFrameMessage = { ...frame, frame: { ...frame.frame, data: frame.frame.data.slice(0) } };
      this.inFlight.set(copy.seq, { mandatory: copy.mandatory, sentAt: this.clock(), byteLength: copy.frame.data.byteLength });
      this.counters.replayedFrames += 1;
      this.post(copy, [copy.frame.data]);
    }
    for (const [id, pending] of this.pending) {
      if (pending.timer !== null) {
        clearTimeout(pending.timer);
        pending.timer = setTimeout(() => { this.pending.delete(id); pending.reject(new Error(`depth fusion ${pending.kind} request timed out`)); }, this.requestTimeoutMs);
      }
      this.post({ type: 'request', generation: this.generation, id, kind: pending.kind, ...(pending.params === undefined ? {} : { params: pending.params }) });
    }
    if (this.keeper === null) this.reportRecovery(-1);
    if (this.inFlight.size === 0) this.flushDrainWaiters();
    return true;
  }

  private reportRecovery(restoredThroughSeq: number): void {
    const recovering = this.recovering;
    if (recovering === null) return;
    this.recovering = null;
    let lostFrames = 0; let lostKeyframes = 0;
    for (const [seq, keyframe] of this.evicted) if (seq > restoredThroughSeq) { lostFrames += 1; if (keyframe) lostKeyframes += 1; }
    this.counters.lostFrames += lostFrames; this.counters.lostKeyframes += lostKeyframes;
    /* Reported once: what was lost now cannot be lost again; what the restored checkpoint holds stays accounted in case it is lost later. */
    for (const seq of [...this.evicted.keys()]) if (seq > restoredThroughSeq) this.evicted.delete(seq);
    this.options.onRecovered?.({ reason: recovering.reason, restoredThroughSeq, replayedFrames: recovering.replayed, lostFrames, lostKeyframes });
  }

  private readonly handleKeeperMessage = (event: MessageEvent<{ type: 'stored' | 'missing'; generation: number; throughSeq?: number }>): void => {
    const message = event.data;
    if (this.disposed || message.generation !== this.generation || message.type !== 'stored' || message.throughSeq === undefined) return;
    this.checkpointPending = false;
    this.storedThroughSeq = Math.max(this.storedThroughSeq, message.throughSeq);
    this.counters.checkpointsStored += 1;
    for (const [seq, frame] of this.journal) if (seq <= this.storedThroughSeq) { this.evicted.set(seq, frame.mandatory); this.journal.delete(seq); }
  };

  private post(message: FusionWorkerRequest, transfer: Transferable[] = []): void {
    if (this.worker !== null) { try { this.worker.postMessage(message, transfer); } catch (error) { this.fail(error instanceof Error ? error.message : String(error)); } return; }
    this.inline?.handle(message);
  }

  /** A dead worker: recover if a replacement can take over, otherwise the failure is terminal. */
  private died(reason: string): void {
    if (this.disposed || this.failed !== null) return;
    if (!this.recover(reason)) this.fail(reason);
  }

  private fail(reason: string): void {
    if (this.failed !== null) return;
    this.failed = reason;
    this.failPending(`depth fusion failed: ${reason}`);
    for (const { byteLength } of this.inFlight.values()) this.pool.release(null, byteLength);
    this.inFlight.clear();
    this.flushDrainWaiters();
    this.releaseWorker();
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
    if (message.type === 'restored') {
      this.clearRestoreTimer();
      this.mirror = message.stats;
      this.reportRecovery(message.throughSeq);
      return;
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
      this.options.onFrameTiming?.(message.workMs, this.inFlight.size);
      this.framesSinceCheckpoint += 1;
      if (this.keeper !== null && !this.checkpointPending && this.framesSinceCheckpoint >= (this.options.checkpointEveryFrames ?? FUSION_DEFAULT_CHECKPOINT_EVERY)) {
        this.framesSinceCheckpoint = 0;
        this.checkpointPending = true;
        this.post({ type: 'checkpoint', generation: this.generation });
      }
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
      if (entry !== undefined) {
        this.inFlight.delete(message.seq); this.pool.release(null, entry.byteLength);
        this.journal.delete(message.seq);                                         /* a frame the fusion refuses would be refused again */
        this.counters.lostFrames += 1; if (entry.mandatory) this.counters.lostKeyframes += 1;
        if (this.inFlight.size === 0) this.flushDrainWaiters();
      }
      return;
    }
    if (message.seq === undefined && message.id === undefined) { this.died(message.message); return; }   /* e.g. a failed restore */
    if (message.id !== undefined) {
      const pending = this.pending.get(message.id);
      if (pending !== undefined) { this.pending.delete(message.id); if (pending.timer !== null) clearTimeout(pending.timer); pending.reject(new Error(message.message)); }
    }
  }

  private readonly handleMessage = (event: MessageEvent<FusionWorkerResponse>): void => { this.deliver(event.data); };
  private readonly handleError = (event: ErrorEvent): void => { this.died(event.message || 'worker error'); };
  private readonly handleMessageError = (): void => { this.died('unreadable worker message'); };
}
