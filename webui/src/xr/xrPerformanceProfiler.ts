export type XrPerfMetric =
  | 'frameWork'
  | 'depthAccess'
  | 'fusion'
  | 'coverage'
  | 'coverageSnapshot'
  | 'coverageUpload'
  | 'coverageDraw';

export interface XrPerformanceSnapshot {
  frameDeltaP50Ms: number;
  frameDeltaP95Ms: number;
  frameDeltaMaxMs: number;
  frameWorkP95Ms: number;
  depthAccessP95Ms: number;
  fusionP50Ms: number;
  fusionP95Ms: number;
  fusionMaxMs: number;
  /** Compatibility aggregate: worst p95 across build/upload/draw coverage work. */
  coverageP95Ms: number;
  coverageSnapshotP95Ms: number;
  coverageUploadP95Ms: number;
  coverageDrawP95Ms: number;
  longFrames33Ms: number;
  longFrames50Ms: number;
  sampleCount: number;
  adaptiveCoverageIntervalMs: number;
  adaptiveCoveragePointBudget: number;
  adaptiveCoverageDrawIntervalMs: number;
  pressure: 'low' | 'medium' | 'high';
}

export interface XrRenderPolicy {
  pointBudget: number;
  drawIntervalMs: number;
}

type XrPressure = XrPerformanceSnapshot['pressure'];

const WINDOW_SIZE = 160;
const DEFAULT_COVERAGE_INTERVAL_MS = 550;
const LOW_PRESSURE_POINT_BUDGET = 12000;
const MEDIUM_PRESSURE_POINT_BUDGET = 8000;
const HIGH_PRESSURE_POINT_BUDGET = 4800;

/**
 * Hot-path policy cache cadence (MAX-409).
 *
 * Reading the render policy happens on every visible XR frame (and the coverage
 * interval is read on every sampled depth frame), so a read must never sort the
 * rolling windows. Pressure is instead re-evaluated from the windows at a
 * bounded cadence that is driven only by data handed to the profiler (the XR
 * frame timestamps and the accepted sample count), never by a hidden wall
 * clock, so a given sample stream always yields the same policy sequence.
 *
 * Worst-case staleness of the cached policy relative to evaluating the windows
 * exactly (what snapshot() does):
 * - frame-clocked (recordFrameTimestamp() is being called, always true in the
 *   XR runtime): the refresh runs on the first frame boundary at least
 *   XR_POLICY_REFRESH_INTERVAL_MS after the previous refresh, so a policy read
 *   is never more than that interval plus one frame period behind the windows;
 * - unclocked (only record() was used since reset()): the refresh runs after
 *   XR_POLICY_UNCLOCKED_REFRESH_SAMPLES accepted samples;
 * - a full snapshot() also refreshes the cache, because it has just evaluated
 *   the very same windows (UI publishes it at 10 Hz).
 * The p95 gates themselves need at least 5% of a 160-sample window (8 samples,
 * ~133 ms at 60 Hz) beyond a threshold before they flip, so the 200 ms bound
 * adds roughly 1.5x to a physical detection latency that already exists. The
 * policy only steers live visualization (point budget on the 550-1100 ms
 * coverage upload cadence, draw interval per frame), never scan evidence.
 * There is deliberately no extra hysteresis: once a refresh runs, transitions
 * are exactly the reference threshold math.
 */
export const XR_POLICY_REFRESH_INTERVAL_MS = 200;
export const XR_POLICY_UNCLOCKED_REFRESH_SAMPLES = 16;

/** Shared frozen policies: a policy read returns one of these, allocation-free. */
const RENDER_POLICY_BY_PRESSURE: Readonly<Record<XrPressure, XrRenderPolicy>> = {
  low: Object.freeze({ pointBudget: LOW_PRESSURE_POINT_BUDGET, drawIntervalMs: 0 }),
  medium: Object.freeze({ pointBudget: MEDIUM_PRESSURE_POINT_BUDGET, drawIntervalMs: 16 }),
  high: Object.freeze({ pointBudget: HIGH_PRESSURE_POINT_BUDGET, drawIntervalMs: 32 }),
};
const COVERAGE_INTERVAL_MS_BY_PRESSURE: Readonly<Record<XrPressure, number>> = {
  low: DEFAULT_COVERAGE_INTERVAL_MS,
  medium: 780,
  high: 1100,
};

interface PressureInputs {
  fusionP95: number;
  frameWorkP95: number;
  frameDeltaP95: number;
  coverageSnapshotP95: number;
  coverageUploadP95: number;
  coverageDrawP95: number;
}

function classifyPressure(inputs: PressureInputs): XrPressure {
  const { fusionP95, frameWorkP95, frameDeltaP95, coverageSnapshotP95, coverageUploadP95, coverageDrawP95 } = inputs;
  if (
    fusionP95 >= 90 || frameWorkP95 >= 32 || frameDeltaP95 >= 42 ||
    coverageSnapshotP95 >= 45 || coverageUploadP95 >= 24 || coverageDrawP95 >= 13
  ) return 'high';
  if (
    fusionP95 >= 55 || frameWorkP95 >= 22 || frameDeltaP95 >= 28 ||
    coverageSnapshotP95 >= 28 || coverageUploadP95 >= 13 || coverageDrawP95 >= 7
  ) return 'medium';
  return 'low';
}

class RollingSamples {
  private readonly values: number[] = [];

  clear(): void { this.values.length = 0; }

  /** Returns whether the sample was accepted into the window. */
  add(value: number): boolean {
    if (!Number.isFinite(value) || value < 0) return false;
    this.values.push(value);
    if (this.values.length > WINDOW_SIZE) this.values.splice(0, this.values.length - WINDOW_SIZE);
    return true;
  }

  percentile(fraction: number): number {
    if (this.values.length === 0) return 0;
    const sorted = [...this.values].sort((a, b) => a - b);
    const index = Math.max(0, Math.min(sorted.length - 1, Math.round((sorted.length - 1) * fraction)));
    return sorted[index];
  }

  max(): number {
    let result = 0;
    for (const value of this.values) result = Math.max(result, value);
    return result;
  }

  countAtLeast(threshold: number): number {
    let count = 0;
    for (const value of this.values) if (value >= threshold) count += 1;
    return count;
  }

  get size(): number { return this.values.length; }
}

let activeProfiler: XrPerformanceProfiler | null = null;
let activeProfilerToken = 0;
let nextProfilerToken = 0;

/**
 * Rolling profiler for the hot WebXR path. It deliberately stores only a small
 * bounded window, so diagnostics cannot become another source of scan jank.
 * Adaptive degradation changes only live visualization; fusion quality and
 * stored scan evidence are never downsampled by this class.
 */
export class XrPerformanceProfiler {
  private readonly frameDelta = new RollingSamples();
  private readonly metrics: Record<XrPerfMetric, RollingSamples> = {
    frameWork: new RollingSamples(),
    depthAccess: new RollingSamples(),
    fusion: new RollingSamples(),
    coverage: new RollingSamples(),
    coverageSnapshot: new RollingSamples(),
    coverageUpload: new RollingSamples(),
    coverageDraw: new RollingSamples(),
  };
  private previousFrameTime: number | null = null;
  // Cached pressure for hot-path policy reads; see XR_POLICY_REFRESH_INTERVAL_MS.
  private cachedPressure: XrPressure = 'low';
  private pendingPolicySamples = 0;
  private policyRefreshFrameMs: number | null = null;

  /** Activate this profiler after its owning XR runtime has committed. */
  activate(): number {
    const token = ++nextProfilerToken;
    activeProfiler = this;
    activeProfilerToken = token;
    return token;
  }

  /** A stale runtime cannot deactivate a profiler activated later. */
  deactivate(token: number): void {
    if (activeProfiler !== this || activeProfilerToken !== token) return;
    activeProfiler = null;
    activeProfilerToken = 0;
  }

  reset(): void {
    this.frameDelta.clear();
    for (const metric of Object.values(this.metrics)) metric.clear();
    this.previousFrameTime = null;
    this.cachedPressure = 'low';
    this.pendingPolicySamples = 0;
    this.policyRefreshFrameMs = null;
  }

  recordFrameTimestamp(timeMs: number): void {
    if (Number.isFinite(timeMs) && this.previousFrameTime !== null) {
      if (this.frameDelta.add(Math.max(0, timeMs - this.previousFrameTime))) this.pendingPolicySamples += 1;
    }
    if (Number.isFinite(timeMs)) {
      this.previousFrameTime = timeMs;
      // Frame boundary: the XR runtime calls this before it starts its frameWork
      // timer, so a due refresh never lands inside a measured window.
      this.refreshPolicyIfDue(timeMs);
    }
  }

  record(metric: XrPerfMetric, durationMs: number): void {
    if (!this.metrics[metric].add(durationMs)) return;
    this.pendingPolicySamples += 1;
    // Without a frame clock there is no boundary to refresh on, so a stream that
    // only uses record() falls back to a sample-count cadence.
    if (this.previousFrameTime === null && this.pendingPolicySamples >= XR_POLICY_UNCLOCKED_REFRESH_SAMPLES) {
      this.refreshPolicy(null);
    }
  }

  /**
   * Full diagnostic snapshot. It evaluates every rolling window and therefore
   * belongs to UI/telemetry cadence, not to per-frame code: hot paths read the
   * cached policy through activeXrRenderPolicy() / the recommended*() helpers.
   */
  snapshot(): XrPerformanceSnapshot {
    const fusionP95 = this.metrics.fusion.percentile(0.95);
    const frameWorkP95 = this.metrics.frameWork.percentile(0.95);
    const frameDeltaP95 = this.frameDelta.percentile(0.95);
    const legacyCoverageP95 = this.metrics.coverage.percentile(0.95);
    const explicitCoverageSnapshotP95 = this.metrics.coverageSnapshot.percentile(0.95);
    const coverageSnapshotP95 = this.metrics.coverageSnapshot.size > 0
      ? explicitCoverageSnapshotP95
      : legacyCoverageP95;
    const coverageUploadP95 = this.metrics.coverageUpload.percentile(0.95);
    const coverageDrawP95 = this.metrics.coverageDraw.percentile(0.95);
    const coverageP95 = Math.max(legacyCoverageP95, coverageSnapshotP95, coverageUploadP95, coverageDrawP95);

    const pressure = classifyPressure({
      fusionP95, frameWorkP95, frameDeltaP95, coverageSnapshotP95, coverageUploadP95, coverageDrawP95,
    });
    const adaptiveCoverageIntervalMs = COVERAGE_INTERVAL_MS_BY_PRESSURE[pressure];
    const adaptiveCoveragePointBudget = RENDER_POLICY_BY_PRESSURE[pressure].pointBudget;
    const adaptiveCoverageDrawIntervalMs = RENDER_POLICY_BY_PRESSURE[pressure].drawIntervalMs;
    // The windows were just evaluated exactly; the hot-path cache may adopt it.
    this.adoptPressure(pressure, this.previousFrameTime);

    return {
      frameDeltaP50Ms: this.frameDelta.percentile(0.50),
      frameDeltaP95Ms: frameDeltaP95,
      frameDeltaMaxMs: this.frameDelta.max(),
      frameWorkP95Ms: frameWorkP95,
      depthAccessP95Ms: this.metrics.depthAccess.percentile(0.95),
      fusionP50Ms: this.metrics.fusion.percentile(0.50),
      fusionP95Ms: fusionP95,
      fusionMaxMs: this.metrics.fusion.max(),
      coverageP95Ms: coverageP95,
      coverageSnapshotP95Ms: coverageSnapshotP95,
      coverageUploadP95Ms: coverageUploadP95,
      coverageDrawP95Ms: coverageDrawP95,
      longFrames33Ms: this.frameDelta.countAtLeast(33),
      longFrames50Ms: this.frameDelta.countAtLeast(50),
      sampleCount: Math.max(this.frameDelta.size, this.metrics.fusion.size),
      adaptiveCoverageIntervalMs,
      adaptiveCoveragePointBudget,
      adaptiveCoverageDrawIntervalMs,
      pressure,
    };
  }

  /** Cached pressure: O(1), allocation-free, at most the documented refresh bound stale. */
  currentPressure(): XrPressure {
    return this.cachedPressure;
  }

  /** Cached render policy: O(1) and allocation-free (returns a shared frozen object). */
  renderPolicy(): XrRenderPolicy {
    return RENDER_POLICY_BY_PRESSURE[this.cachedPressure];
  }

  recommendedCoverageIntervalMs(): number {
    return COVERAGE_INTERVAL_MS_BY_PRESSURE[this.cachedPressure];
  }

  recommendedCoveragePointBudget(): number {
    return RENDER_POLICY_BY_PRESSURE[this.cachedPressure].pointBudget;
  }

  recommendedCoverageDrawIntervalMs(): number {
    return RENDER_POLICY_BY_PRESSURE[this.cachedPressure].drawIntervalMs;
  }

  private refreshPolicyIfDue(frameTimeMs: number): void {
    if (this.policyRefreshFrameMs === null) {
      // First frame since reset: anchor the cadence; the windows are still tiny.
      this.policyRefreshFrameMs = frameTimeMs;
      return;
    }
    if (this.pendingPolicySamples === 0) return;
    const elapsedMs = frameTimeMs - this.policyRefreshFrameMs;
    // A frame clock that moved backwards (new XR session) refreshes immediately.
    if (elapsedMs >= 0 && elapsedMs < XR_POLICY_REFRESH_INTERVAL_MS) return;
    this.refreshPolicy(frameTimeMs);
  }

  /** Re-evaluates only the windows that decide pressure (no full snapshot). */
  private refreshPolicy(frameTimeMs: number | null): void {
    const coverageSnapshotP95 = this.metrics.coverageSnapshot.size > 0
      ? this.metrics.coverageSnapshot.percentile(0.95)
      : this.metrics.coverage.percentile(0.95);
    this.adoptPressure(classifyPressure({
      fusionP95: this.metrics.fusion.percentile(0.95),
      frameWorkP95: this.metrics.frameWork.percentile(0.95),
      frameDeltaP95: this.frameDelta.percentile(0.95),
      coverageSnapshotP95,
      coverageUploadP95: this.metrics.coverageUpload.percentile(0.95),
      coverageDrawP95: this.metrics.coverageDraw.percentile(0.95),
    }), frameTimeMs);
  }

  private adoptPressure(pressure: XrPressure, frameTimeMs: number | null): void {
    this.cachedPressure = pressure;
    this.pendingPolicySamples = 0;
    this.policyRefreshFrameMs = frameTimeMs;
  }
}

export function recordActiveXrPerformanceMetric(metric: XrPerfMetric, durationMs: number): void {
  activeProfiler?.record(metric, durationMs);
}

/**
 * Scheduler-facing live pressure hint. It intentionally reuses the same bounded
 * rolling profiler as renderer adaptation instead of running a CPU benchmark or
 * a second telemetry loop. It reads the cached pressure, so a heavy job asking
 * for a lease never triggers a profiler pass.
 */
export function activeXrPerformancePressure(): XrPerformanceSnapshot['pressure'] {
  return activeProfiler?.currentPressure() ?? 'low';
}

/**
 * Hot-path policy read, called on every rendered XR frame: returns the cached
 * policy in O(1) with no sorting and no allocation. The cache is refreshed at a
 * bounded cadence by the profiler itself (see XR_POLICY_REFRESH_INTERVAL_MS).
 */
export function activeXrRenderPolicy(): XrRenderPolicy {
  return activeProfiler === null ? RENDER_POLICY_BY_PRESSURE.low : activeProfiler.renderPolicy();
}

export const emptyXrPerformanceSnapshot: XrPerformanceSnapshot = {
  frameDeltaP50Ms: 0,
  frameDeltaP95Ms: 0,
  frameDeltaMaxMs: 0,
  frameWorkP95Ms: 0,
  depthAccessP95Ms: 0,
  fusionP50Ms: 0,
  fusionP95Ms: 0,
  fusionMaxMs: 0,
  coverageP95Ms: 0,
  coverageSnapshotP95Ms: 0,
  coverageUploadP95Ms: 0,
  coverageDrawP95Ms: 0,
  longFrames33Ms: 0,
  longFrames50Ms: 0,
  sampleCount: 0,
  adaptiveCoverageIntervalMs: DEFAULT_COVERAGE_INTERVAL_MS,
  adaptiveCoveragePointBudget: LOW_PRESSURE_POINT_BUDGET,
  adaptiveCoverageDrawIntervalMs: 0,
  pressure: 'low',
};
