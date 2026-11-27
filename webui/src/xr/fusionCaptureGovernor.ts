import type { FusionFrameDecision, FusionPoseLike, FusionSelectorMark, WebXrFusionKeyframeSelector } from './webxrFusionQuality';

/**
 * Admission control of depth capture (Phase 2): the rate is regulated BEFORE a frame is captured and queued, so that nothing accepted is ever discarded.
 *
 * Frames are ranked by how much new evidence they can carry, and each rank may only use so much of the fusion queue:
 *   keyframe  - an informative new viewpoint: may use every slot, including the reserve;
 *   depth     - the camera has moved away from the last keyframe but not yet far enough: only the ordinary slots;
 *   polish    - small motion around the last keyframe: only while at most one frame is waiting;
 *   repeat    - the same view again (camera stationary): only while the worker is idle.
 * A frame that is refused is not captured at all and the keyframe selector is put back, so the same viewpoint stays a candidate for the next XR frame:
 * a refusal delays evidence, it does not mark the view as taken. The sampling interval follows the measured fusion time of the worker, with hysteresis
 * (it widens at once and narrows only after a run of frames with a short queue). While not even a keyframe can be taken, `catchingUp` is raised for
 * the operator; it drops again only once the queue has drained to one frame.
 *
 * The measured interval paces the frames that only refine what is already seen. A KEYFRAME is not held to it while the worker has no backlog: the selector is
 * asked at the base cadence and an informative viewpoint is taken at once, because asking it less often spaces the keyframes further apart and loses surface
 * even though the worker is idle most of the time (desktop replay, +200 ms per frame: 74 keyframes instead of 101, recall 0.554 instead of 0.612). Once
 * `maxInFlight` frames are waiting, keyframes are paced too, which keeps the delay to the live mesh and to Finish short.
 */
export type FusionFrameClass = 'keyframe' | 'depth' | 'polish' | 'repeat';

export const FUSION_BASE_INTERVAL_MS = 140;
export const FUSION_MAX_INTERVAL_MS = 1500;
/** The interval keeps this much headroom over the measured fusion time per frame. */
const THROUGHPUT_HEADROOM = 1.2;
const EWMA_ALPHA = 0.25;
/** Frames in a row finished with a short queue before the interval may narrow. */
const NARROW_AFTER = 6;
const NARROW_STEP = 0.85;
const WIDEN_STEP = 1.3;
/** A pose this far from the last keyframe is already adding coverage ('depth'), not just refining it. */
const DEPTH_TRANSLATION_M = 0.025;
const DEPTH_ROTATION_RAD = (6 * Math.PI) / 180;

export interface FusionQueueView {
  /** Frames accepted and not yet fused. */
  queueDepth: number;
  maxInFlight: number;
  mandatoryReserve: number;
}

export interface FusionCaptureCounters {
  captured: Record<FusionFrameClass, number>;
  /** Refused by the policy before capture; the view stays a candidate. */
  deferred: Record<FusionFrameClass, number>;
  /** XR frames at which no slot at all was free, so not even the selector was consulted. */
  deferredNoSlot: number;
  catchingUpEntries: number;
}

export type FusionCaptureStep =
  | { action: 'not-due' }
  | { action: 'no-slot' }
  | { action: 'skip'; decision: FusionFrameDecision }
  | { action: 'deferred'; decision: FusionFrameDecision; frameClass: FusionFrameClass }
  | { action: 'capture'; decision: FusionFrameDecision; frameClass: FusionFrameClass; mark: FusionSelectorMark };

export function classifyFusionFrame(decision: FusionFrameDecision): FusionFrameClass {
  if (decision.kind === 'keyframe') return 'keyframe';
  if (decision.state === 'stationary') return 'repeat';
  return decision.translationFromKeyframeMeters >= DEPTH_TRANSLATION_M || decision.rotationFromKeyframeRadians >= DEPTH_ROTATION_RAD ? 'depth' : 'polish';
}

export function slotsFor(frameClass: FusionFrameClass, queue: FusionQueueView): number {
  switch (frameClass) {
    case 'keyframe': return queue.maxInFlight + queue.mandatoryReserve;
    case 'depth': return queue.maxInFlight;
    case 'polish': return Math.min(2, queue.maxInFlight);
    case 'repeat': return 1;
  }
}

const zero = (): Record<FusionFrameClass, number> => ({ keyframe: 0, depth: 0, polish: 0, repeat: 0 });

export class FusionCaptureGovernor {
  private interval = FUSION_BASE_INTERVAL_MS;
  private workEwmaMs = 0;
  private shortQueueRun = 0;
  private lastCaptureMs = Number.NEGATIVE_INFINITY;
  /** When the selector was last asked and its answer used. */
  private lastAskedMs = Number.NEGATIVE_INFINITY;
  private catching = false;
  counters: FusionCaptureCounters = { captured: zero(), deferred: zero(), deferredNoSlot: 0, catchingUpEntries: 0 };

  reset(): void {
    this.interval = FUSION_BASE_INTERVAL_MS; this.workEwmaMs = 0; this.shortQueueRun = 0; this.lastCaptureMs = Number.NEGATIVE_INFINITY; this.lastAskedMs = Number.NEGATIVE_INFINITY; this.catching = false;
    this.counters = { captured: zero(), deferred: zero(), deferredNoSlot: 0, catchingUpEntries: 0 };
  }

  get intervalMs(): number { return this.interval; }
  get measuredWorkMs(): number { return this.workEwmaMs; }
  /** True while the device cannot take even a new viewpoint: the operator should hold still. */
  get catchingUp(): boolean { return this.catching; }

  /** The worker finished a frame: its fusion time and what is still waiting drive the sampling interval. */
  frameDone(workMs: number, queue: FusionQueueView): void {
    if (Number.isFinite(workMs) && workMs >= 0) this.workEwmaMs = this.workEwmaMs === 0 ? workMs : this.workEwmaMs + (workMs - this.workEwmaMs) * EWMA_ALPHA;
    const floor = Math.min(FUSION_MAX_INTERVAL_MS, Math.max(FUSION_BASE_INTERVAL_MS, this.workEwmaMs * THROUGHPUT_HEADROOM));
    if (queue.queueDepth >= queue.maxInFlight) {
      this.shortQueueRun = 0;
      this.interval = Math.min(FUSION_MAX_INTERVAL_MS, Math.max(floor, this.interval * WIDEN_STEP));
    } else if (queue.queueDepth <= 1) {
      this.shortQueueRun += 1;
      if (this.shortQueueRun >= NARROW_AFTER) { this.shortQueueRun = 0; this.interval = this.interval * NARROW_STEP; }
    } else this.shortQueueRun = 0;
    this.interval = Math.min(FUSION_MAX_INTERVAL_MS, Math.max(floor, this.interval));
    if (this.catching && queue.queueDepth <= 1) this.catching = false;
  }

  /**
   * One XR frame. Decides whether depth is captured now; on 'capture' the caller captures and submits, and calls {@link refuse} with that step if the
   * hand-off fails. The selector is only consulted when a frame is due and a slot exists, and is put back when the frame is deferred. A keyframe may use
   * every slot, so it is never deferred: it either finds a slot or the whole XR frame is 'no-slot'.
   */
  step(timeMs: number, pose: FusionPoseLike, selector: WebXrFusionKeyframeSelector, queue: FusionQueueView): FusionCaptureStep {
    const backlog = queue.queueDepth >= queue.maxInFlight;
    const paced = timeMs - this.lastCaptureMs >= this.interval;
    if (timeMs - this.lastAskedMs < FUSION_BASE_INTERVAL_MS || (backlog && !paced)) return { action: 'not-due' };
    if (queue.queueDepth >= queue.maxInFlight + queue.mandatoryReserve) {
      this.counters.deferredNoSlot += 1;
      this.enterCatchingUp();
      return { action: 'no-slot' };
    }
    const mark = selector.mark();
    const decision = selector.evaluate(timeMs, pose);
    if (decision.kind === 'skip') { this.lastAskedMs = timeMs; return { action: 'skip', decision }; }
    const frameClass = classifyFusionFrame(decision);
    if (frameClass !== 'keyframe' && !paced) {
      selector.withdraw(mark);
      this.lastAskedMs = timeMs;
      return { action: 'not-due' };
    }
    if (queue.queueDepth >= slotsFor(frameClass, queue)) {
      selector.withdraw(mark);
      this.counters.deferred[frameClass] += 1;
      return { action: 'deferred', decision, frameClass };
    }
    this.lastCaptureMs = timeMs;
    this.lastAskedMs = timeMs;
    this.counters.captured[frameClass] += 1;
    return { action: 'capture', decision, frameClass, mark };
  }

  /** The fusion did not take a frame {@link step} had admitted: its viewpoint is not taken and the selector offers it again. */
  refuse(step: Extract<FusionCaptureStep, { action: 'capture' }>, selector: WebXrFusionKeyframeSelector): void {
    selector.withdraw(step.mark);
    this.counters.captured[step.frameClass] -= 1;
  }

  private enterCatchingUp(): void {
    if (!this.catching) { this.catching = true; this.counters.catchingUpEntries += 1; }
  }
}
