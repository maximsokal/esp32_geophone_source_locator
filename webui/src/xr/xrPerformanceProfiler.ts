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
  /** Runtime coverage-build envelope; on legacy call sites this also includes upload. */
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

const WINDOW_SIZE = 160;
const DEFAULT_COVERAGE_INTERVAL_MS = 550;
const LOW_PRESSURE_POINT_BUDGET = 12000;
const MEDIUM_PRESSURE_POINT_BUDGET = 8000;
const HIGH_PRESSURE_POINT_BUDGET = 4800;

class RollingSamples {
  private readonly values: number[] = [];

  clear(): void { this.values.length = 0; }

  add(value: number): void {
    if (!Number.isFinite(value) || value < 0) return;
    this.values.push(value);
    if (this.values.length > WINDOW_SIZE) this.values.splice(0, this.values.length - WINDOW_SIZE);
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

  constructor() {
    // There is one immersive XR runtime per page. Renderer instrumentation uses
    // this bounded active sink so upload/draw timings do not require callbacks in
    // the frame hot path.
    activeProfiler = this;
  }

  reset(): void {
    this.frameDelta.clear();
    for (const metric of Object.values(this.metrics)) metric.clear();
    this.previousFrameTime = null;
    activeProfiler = this;
  }

  recordFrameTimestamp(timeMs: number): void {
    if (Number.isFinite(timeMs) && this.previousFrameTime !== null) {
      this.frameDelta.add(Math.max(0, timeMs - this.previousFrameTime));
    }
    if (Number.isFinite(timeMs)) this.previousFrameTime = timeMs;
  }

  record(metric: XrPerfMetric, durationMs: number): void {
    this.metrics[metric].add(durationMs);
  }

  snapshot(): XrPerformanceSnapshot {
    const fusionP95 = this.metrics.fusion.percentile(0.95);
    const frameWorkP95 = this.metrics.frameWork.percentile(0.95);
    const frameDeltaP95 = this.frameDelta.percentile(0.95);
    const legacyCoverageP95 = this.metrics.coverage.percentile(0.95);
    const explicitCoverageSnapshotP95 = this.metrics.coverageSnapshot.percentile(0.95);
    const coverageSnapshotP95 = Math.max(explicitCoverageSnapshotP95, legacyCoverageP95);
    const coverageUploadP95 = this.metrics.coverageUpload.percentile(0.95);
    const coverageDrawP95 = this.metrics.coverageDraw.percentile(0.95);
    const coverageP95 = Math.max(coverageSnapshotP95, coverageUploadP95, coverageDrawP95);

    let pressure: XrPerformanceSnapshot['pressure'] = 'low';
    if (
      fusionP95 >= 90 || frameWorkP95 >= 32 || frameDeltaP95 >= 42 ||
      coverageSnapshotP95 >= 45 || coverageUploadP95 >= 24 || coverageDrawP95 >= 13
    ) pressure = 'high';
    else if (
      fusionP95 >= 55 || frameWorkP95 >= 22 || frameDeltaP95 >= 28 ||
      coverageSnapshotP95 >= 28 || coverageUploadP95 >= 13 || coverageDrawP95 >= 7
    ) pressure = 'medium';

    const adaptiveCoverageIntervalMs = pressure === 'high'
      ? 1100
      : pressure === 'medium'
        ? 780
        : DEFAULT_COVERAGE_INTERVAL_MS;
    const adaptiveCoveragePointBudget = pressure === 'high'
      ? HIGH_PRESSURE_POINT_BUDGET
      : pressure === 'medium'
        ? MEDIUM_PRESSURE_POINT_BUDGET
        : LOW_PRESSURE_POINT_BUDGET;
    const adaptiveCoverageDrawIntervalMs = pressure === 'high' ? 32 : pressure === 'medium' ? 16 : 0;

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

  recommendedCoverageIntervalMs(): number {
    return this.snapshot().adaptiveCoverageIntervalMs;
  }

  recommendedCoveragePointBudget(): number {
    return this.snapshot().adaptiveCoveragePointBudget;
  }

  recommendedCoverageDrawIntervalMs(): number {
    return this.snapshot().adaptiveCoverageDrawIntervalMs;
  }
}

export function recordActiveXrPerformanceMetric(metric: XrPerfMetric, durationMs: number): void {
  activeProfiler?.record(metric, durationMs);
}

export function activeXrRenderPolicy(): XrRenderPolicy {
  if (activeProfiler === null) return { pointBudget: LOW_PRESSURE_POINT_BUDGET, drawIntervalMs: 0 };
  return {
    pointBudget: activeProfiler.recommendedCoveragePointBudget(),
    drawIntervalMs: activeProfiler.recommendedCoverageDrawIntervalMs(),
  };
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
