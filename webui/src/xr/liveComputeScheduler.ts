import { deviceComputeProfilePromise } from './deviceComputeProfile';
import { activeXrPerformancePressure } from './xrPerformanceProfiler';

export type LiveComputeTaskKind = 'structural' | 'visual';
export type LiveComputePressure = 'low' | 'medium' | 'high';

export interface LiveComputeLease {
  readonly kind: LiveComputeTaskKind;
  /** Release after the worker job settles or its worker has been terminated. */
  release(): void;
}

export interface LiveComputeSchedulerSnapshot {
  /** Backward-compatible alias for effectiveBudget. */
  budget: number | null;
  /** Fixed pre-scan hardware/capability ceiling. */
  maximumBudget: number | null;
  /** Current live ceiling after thermal/frame-pressure hysteresis. */
  effectiveBudget: number | null;
  pressure: LiveComputePressure;
  degraded: boolean;
  /** Admitted CPU jobs; epoch invalidation alone does not make a job inactive. */
  active: number;
  structural: number;
  visual: number;
}

type BudgetProvider = () => Promise<number>;
type PressureProvider = () => LiveComputePressure;
type Clock = () => number;

const PRESSURE_SAMPLE_INTERVAL_MS = 750;
const MEDIUM_SAMPLES_TO_DEGRADE = 3;
const LOW_SAMPLES_TO_RECOVER = 5;

function safeBudget(value: number): number {
  return Number.isFinite(value) ? Math.max(0, Math.floor(value)) : 0;
}

function defaultClock(): number {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

/**
 * Global concurrency gate for CPU-heavy work while immersive XR is live.
 *
 * The pre-scan device profile is a CAPACITY CEILING, not a promise that all of
 * those workers should stay busy for the whole scan. Runtime XR frame pressure
 * applies a hysteretic effective cap: high pressure degrades immediately, while
 * recovery requires several healthy windows so a thermally throttling phone
 * cannot flap between W1 and W2.
 *
 * Structural geometry is the critical live branch. Visual/Hough work is optional
 * corroboration and may use only spare heavy-worker capacity. With W1 there is no
 * spare slot, so visual work is dropped rather than stealing the only slot from a
 * later structural keyframe. With W2, at most one visual job can run and one slot
 * remains available to structural work. Acquisition never queues stale jobs.
 */
export class LiveComputeScheduler {
  private active = 0;
  private activeStructural = 0;
  private activeVisual = 0;
  private resolvedMaximumBudget: number | null = null;
  private maximumBudgetPromise: Promise<number> | null = null;
  private effectiveBudget: number | null = null;
  private pressure: LiveComputePressure = 'low';
  private pressureCap: number | null = null;
  private mediumSamples = 0;
  private lowRecoverySamples = 0;
  private lastHysteresisSampleMs = Number.NEGATIVE_INFINITY;

  constructor(
    private readonly budgetProvider: BudgetProvider,
    private readonly pressureProvider: PressureProvider = () => 'low',
    private readonly clock: Clock = defaultClock,
  ) {}

  /**
   * Start a fresh scan window without pretending the device thermally recovered.
   *
   * Medium/low streak counters belong to one XR session and are reset. The last
   * safety cap is deliberately preserved: ending and immediately restarting AR
   * does not cool a phone. Fresh low-pressure telemetry must earn W2 back through
   * the ordinary recovery hysteresis. Outstanding leases are also left alone;
   * scan lifecycle code must reset/terminate their owning clients explicitly.
   */
  beginScan(): void {
    this.mediumSamples = 0;
    this.lowRecoverySamples = 0;
    this.lastHysteresisSampleMs = Number.NEGATIVE_INFINITY;
    this.refreshEffectiveBudget();
  }

  /**
   * Resolve the fixed hardware/capability ceiling before immersive XR starts.
   * This is negotiation only: the budget provider reads browser capability hints
   * and never runs a CPU benchmark. Runtime pressure can subsequently reduce the
   * effective budget but cannot exceed this pre-scan maximum.
   */
  async prepare(): Promise<LiveComputeSchedulerSnapshot> {
    await this.resolveMaximumBudget();
    this.refreshEffectiveBudget();
    return this.snapshot();
  }

  /**
   * Feed runtime pressure into the live cap. High pressure is intentionally
   * immediate. Medium/low transitions are sampled at a bounded cadence so the
   * 100-ms UI snapshot loop cannot accidentally count as five independent
   * recovery windows in half a second.
   */
  observeRuntimePressure(pressure: LiveComputePressure, timeMs = this.clock()): void {
    if (pressure !== 'low' && pressure !== 'medium' && pressure !== 'high') return;
    this.pressure = pressure;

    if (pressure === 'high') {
      this.pressureCap = 1;
      this.mediumSamples = 0;
      this.lowRecoverySamples = 0;
      this.lastHysteresisSampleMs = Number.isFinite(timeMs) ? timeMs : this.lastHysteresisSampleMs;
      this.refreshEffectiveBudget();
      return;
    }

    if (!Number.isFinite(timeMs) || timeMs - this.lastHysteresisSampleMs < PRESSURE_SAMPLE_INTERVAL_MS) {
      this.refreshEffectiveBudget();
      return;
    }
    this.lastHysteresisSampleMs = timeMs;

    if (pressure === 'medium') {
      this.lowRecoverySamples = 0;
      this.mediumSamples += 1;
      if (this.mediumSamples >= MEDIUM_SAMPLES_TO_DEGRADE) this.pressureCap = 1;
      this.refreshEffectiveBudget();
      return;
    }

    this.mediumSamples = 0;
    if (this.pressureCap === null) {
      this.lowRecoverySamples = 0;
      this.refreshEffectiveBudget();
      return;
    }
    this.lowRecoverySamples += 1;
    if (this.lowRecoverySamples >= LOW_SAMPLES_TO_RECOVER) {
      this.pressureCap = null;
      this.lowRecoverySamples = 0;
    }
    this.refreshEffectiveBudget();
  }

  async tryAcquire(kind: LiveComputeTaskKind): Promise<LiveComputeLease | null> {
    // Pressure is sampled when a heavy branch actually wants CPU. This avoids a
    // second profiler pass on every XR frame while still reacting before work is
    // dispatched to a Worker.
    try {
      this.observeRuntimePressure(this.pressureProvider(), this.clock());
    } catch {
      // Performance telemetry is advisory; a broken provider must not fabricate
      // extra concurrency. Keep the last effective cap.
    }

    const maximumBudget = await this.resolveMaximumBudget();
    this.refreshEffectiveBudget();
    const budget = this.effectiveBudget ?? 0;
    if (maximumBudget <= 0 || budget <= 0 || this.active >= budget) return null;

    if (kind === 'visual') {
      // One logical heavy slot is always reserved for structural geometry. This
      // prevents optional Hough from winning a race and making a structural
      // keyframe miss its only W1 slot.
      const visualCapacity = Math.max(0, budget - 1);
      if (visualCapacity <= 0 || this.activeVisual >= visualCapacity) return null;
    }

    // Promise continuations execute serially on the JS event loop, so this
    // check+increment is atomic with respect to competing main-thread callers.
    this.active += 1;
    if (kind === 'structural') this.activeStructural += 1;
    else this.activeVisual += 1;

    let released = false;
    return {
      kind,
      release: () => {
        if (released) return;
        released = true;
        this.active = Math.max(0, this.active - 1);
        if (kind === 'structural') this.activeStructural = Math.max(0, this.activeStructural - 1);
        else this.activeVisual = Math.max(0, this.activeVisual - 1);
      },
    };
  }

  snapshot(): LiveComputeSchedulerSnapshot {
    return {
      budget: this.effectiveBudget,
      maximumBudget: this.resolvedMaximumBudget,
      effectiveBudget: this.effectiveBudget,
      pressure: this.pressure,
      degraded: this.resolvedMaximumBudget !== null && this.effectiveBudget !== null
        ? this.effectiveBudget < this.resolvedMaximumBudget
        : this.pressureCap !== null,
      active: this.active,
      structural: this.activeStructural,
      visual: this.activeVisual,
    };
  }

  private resolveMaximumBudget(): Promise<number> {
    if (this.resolvedMaximumBudget !== null) return Promise.resolve(this.resolvedMaximumBudget);
    if (this.maximumBudgetPromise === null) {
      this.maximumBudgetPromise = this.budgetProvider()
        .then((value) => safeBudget(value))
        .catch(() => 0)
        .then((value) => {
          this.resolvedMaximumBudget = value;
          this.refreshEffectiveBudget();
          return value;
        });
    }
    return this.maximumBudgetPromise;
  }

  private refreshEffectiveBudget(): void {
    if (this.resolvedMaximumBudget === null) return;
    const cap = this.pressureCap ?? this.resolvedMaximumBudget;
    this.effectiveBudget = Math.min(this.resolvedMaximumBudget, Math.max(0, cap));
  }
}

export const liveComputeScheduler = new LiveComputeScheduler(
  async () => (await deviceComputeProfilePromise).liveWorkerBudget,
  activeXrPerformancePressure,
);
