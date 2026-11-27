import type { LiveComputePressure } from './liveComputeScheduler';

/**
 * How often one periodic live task (coverage guide, live mesh, AprilTag capture) may run (Phase 2, stage 1 §5).
 *
 * The interval is the largest of three things and never leaves [baseMs, maxMs]:
 *  - MEASURED THROUGHPUT: the task's own cost (EWMA) divided by the share of time it may take, so a task that got slower runs less often instead of
 *    piling work onto a busy worker;
 *  - SUSTAINED PRESSURE from the shared scheduler (already hysteretic: see LiveComputeScheduler.sustainedPressure);
 *  - a BACKLOG of the depth fusion, which outranks the task: coverage and live-mesh input are produced by the fusion worker itself.
 * Backing off is immediate. Speeding up needs several calm evaluations and then happens in steps, so the rate does not flap around a boundary.
 */
export interface AdaptiveCadenceOptions {
  baseMs: number;
  maxMs: number;
  /** Share of wall time the task's measured cost may take (0..1]; the interval is at least cost / dutyShare. */
  dutyShare: number;
  /** Multiplier of the base interval at each sustained pressure level. */
  pressureFactor: Readonly<Record<LiveComputePressure, number>>;
  /** Multiplier while the depth fusion is behind; 1 = the task does not yield to it. */
  backlogFactor: number;
}

export interface CadenceLoad {
  pressure: LiveComputePressure;
  fusionBehind: boolean;
}

const COST_WEIGHT = 0.3;
/** A target within this band of the current interval changes nothing. */
const DEAD_BAND = 0.15;
export const CADENCE_CALM_EVALUATIONS = 3;
export const CADENCE_EVALUATION_SPACING_MS = 750;
const RECOVERY_STEP = 0.5;

export class AdaptiveCadence {
  private costEwmaMs = 0;
  private current: number;
  private calm = 0;
  private lastCalmMs = Number.NEGATIVE_INFINITY;
  private changeCount = 0;

  constructor(private readonly options: AdaptiveCadenceOptions) {
    this.current = options.baseMs;
  }

  /** A new XR session: measurements of the previous one say nothing about this one. */
  reset(): void {
    this.costEwmaMs = 0; this.current = this.options.baseMs; this.calm = 0; this.lastCalmMs = Number.NEGATIVE_INFINITY; this.changeCount = 0;
  }

  /** One measured run of the task. */
  recordCost(costMs: number): void {
    if (!Number.isFinite(costMs) || costMs < 0) return;
    this.costEwmaMs = this.costEwmaMs === 0 ? costMs : this.costEwmaMs + COST_WEIGHT * (costMs - this.costEwmaMs);
  }

  get measuredCostMs(): number { return this.costEwmaMs; }
  /** How often the interval has changed since reset (a stability figure for the telemetry). */
  get changes(): number { return this.changeCount; }
  get currentMs(): number { return this.current; }

  target(load: CadenceLoad): number {
    const { baseMs, maxMs, dutyShare, pressureFactor, backlogFactor } = this.options;
    const wanted = Math.max(baseMs * pressureFactor[load.pressure], load.fusionBehind ? baseMs * backlogFactor : baseMs, this.costEwmaMs / dutyShare);
    return Math.min(maxMs, Math.max(baseMs, wanted));
  }

  /** The interval to use now. Safe to call every XR frame. */
  intervalMs(nowMs: number, load: CadenceLoad): number {
    const target = this.target(load);
    if (target > this.current * (1 + DEAD_BAND) || (target === this.options.maxMs && target > this.current)) {
      this.set(target);
      this.calm = 0;
    } else if (target < this.current * (1 - DEAD_BAND) || (target === this.options.baseMs && target < this.current)) {
      if (nowMs - this.lastCalmMs >= CADENCE_EVALUATION_SPACING_MS || nowMs < this.lastCalmMs) {
        this.lastCalmMs = nowMs;
        this.calm += 1;
        if (this.calm >= CADENCE_CALM_EVALUATIONS) {
          const next = Math.max(target, this.current * RECOVERY_STEP);
          this.set(next <= target * (1 + DEAD_BAND) ? target : next);
          this.calm = 0;
        }
      }
    } else this.calm = 0;
    return this.current;
  }

  private set(value: number): void {
    if (value === this.current) return;
    this.current = value;
    this.changeCount += 1;
  }
}

/** The live tasks, least important first to yield: live mesh, then the coverage guide, then AprilTag capture (tags tie a scan to its saved one). */
export const LIVE_MESH_CADENCE: AdaptiveCadenceOptions = { baseMs: 1800, maxMs: 6300, dutyShare: 0.25, pressureFactor: { low: 1, medium: 2, high: 3.5 }, backlogFactor: 2 };
export const COVERAGE_CADENCE: AdaptiveCadenceOptions = { baseMs: 550, maxMs: 2200, dutyShare: 0.1, pressureFactor: { low: 1, medium: 780 / 550, high: 2 }, backlogFactor: 2 };
export const TAG_CADENCE: AdaptiveCadenceOptions = { baseMs: 330, maxMs: 1000, dutyShare: 0.9, pressureFactor: { low: 1, medium: 1, high: 1.5 }, backlogFactor: 1 };
