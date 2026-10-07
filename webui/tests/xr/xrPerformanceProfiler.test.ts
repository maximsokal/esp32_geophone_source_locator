import { describe, expect, it } from 'vitest';

import {
  XR_POLICY_REFRESH_INTERVAL_MS,
  XR_POLICY_UNCLOCKED_REFRESH_SAMPLES,
  XrPerformanceProfiler,
  activeXrPerformancePressure,
  activeXrRenderPolicy,
  type XrPerfMetric,
  type XrPerformanceSnapshot,
} from '../../src/xr/xrPerformanceProfiler';

describe('XrPerformanceProfiler', () => {
  it('keeps frame timing bounded and reports low-pressure full-detail policy', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 220; index += 1) {
      profiler.recordFrameTimestamp(index * 16.67);
      profiler.record('fusion', 18 + (index % 7));
      profiler.record('coverage', 8 + (index % 5));
      profiler.record('coverageUpload', 2 + (index % 2));
      profiler.record('coverageDraw', 1.5);
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.sampleCount).toBeLessThanOrEqual(160);
    expect(snapshot.frameDeltaP95Ms).toBeGreaterThan(16);
    expect(snapshot.frameDeltaP95Ms).toBeLessThan(18);
    expect(snapshot.pressure).toBe('low');
    expect(snapshot.adaptiveCoverageIntervalMs).toBe(550);
    expect(snapshot.adaptiveCoveragePointBudget).toBe(12000);
    expect(snapshot.adaptiveCoverageDrawIntervalMs).toBe(0);
    expect(snapshot.coverageUploadP95Ms).toBeGreaterThan(0);
    expect(snapshot.coverageDrawP95Ms).toBeGreaterThan(0);
  });

  it('reduces only live-render detail under medium pressure', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 80; index += 1) {
      profiler.recordFrameTimestamp(index * 30);
      profiler.record('fusion', 62);
      profiler.record('coverage', 31);
      profiler.record('coverageUpload', 14);
      profiler.record('coverageDraw', 8);
      profiler.record('frameWork', 24);
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.pressure).toBe('medium');
    expect(snapshot.adaptiveCoverageIntervalMs).toBe(780);
    expect(snapshot.adaptiveCoveragePointBudget).toBe(8000);
    expect(snapshot.adaptiveCoverageDrawIntervalMs).toBe(16);
  });

  it('degrades render budget and cadence aggressively under high pressure', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 80; index += 1) {
      profiler.recordFrameTimestamp(index * 46);
      profiler.record('fusion', 105);
      profiler.record('coverage', 55);
      profiler.record('coverageUpload', 28);
      profiler.record('coverageDraw', 15);
      profiler.record('frameWork', 38);
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.pressure).toBe('high');
    expect(snapshot.adaptiveCoverageIntervalMs).toBe(1100);
    expect(snapshot.adaptiveCoveragePointBudget).toBe(4800);
    expect(snapshot.adaptiveCoverageDrawIntervalMs).toBe(32);
    expect(snapshot.longFrames33Ms).toBeGreaterThan(0);
    expect(snapshot.longFrames50Ms).toBe(0);
  });

  it('can raise pressure from draw/upload cost even when fusion is healthy', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 40; index += 1) {
      profiler.recordFrameTimestamp(index * 16.7);
      profiler.record('fusion', 20);
      profiler.record('coverageUpload', 26);
      profiler.record('coverageDraw', 14);
    }
    expect(profiler.snapshot().pressure).toBe('high');
  });

  it('reset clears historical pressure and restores full-detail rendering', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 20; index += 1) {
      profiler.recordFrameTimestamp(index * 60);
      profiler.record('fusion', 120);
    }
    expect(profiler.snapshot().pressure).toBe('high');
    profiler.reset();
    const snapshot = profiler.snapshot();
    expect(snapshot.pressure).toBe('low');
    expect(snapshot.sampleCount).toBe(0);
    expect(snapshot.adaptiveCoveragePointBudget).toBe(12000);
    expect(snapshot.adaptiveCoverageDrawIntervalMs).toBe(0);
  });
});

/*
 * ORACLE: the pre-MAX-409 policy math, restated independently of the production
 * code. Every percentile sorts a copy of the 160-sample window and pressure comes
 * from the six p95 gates exactly as XrPerformanceProfiler.snapshot() computed it
 * on every policy read. The cached policy must converge to this oracle.
 */
type OracleMetric = XrPerfMetric | 'frameDelta';

class OldSnapshotMathOracle {
  private readonly windows: Record<OracleMetric, number[]> = {
    frameDelta: [], frameWork: [], depthAccess: [], fusion: [],
    coverage: [], coverageSnapshot: [], coverageUpload: [], coverageDraw: [],
  };
  private previousFrameTime: number | null = null;

  frame(timeMs: number): void {
    if (this.previousFrameTime !== null) this.push('frameDelta', Math.max(0, timeMs - this.previousFrameTime));
    this.previousFrameTime = timeMs;
  }

  push(metric: OracleMetric, value: number): void {
    if (!Number.isFinite(value) || value < 0) return;
    const window = this.windows[metric];
    window.push(value);
    if (window.length > 160) window.splice(0, window.length - 160);
  }

  private percentile(metric: OracleMetric, fraction: number): number {
    const window = this.windows[metric];
    if (window.length === 0) return 0;
    const sorted = [...window].sort((a, b) => a - b);
    const index = Math.max(0, Math.min(sorted.length - 1, Math.round((sorted.length - 1) * fraction)));
    return sorted[index];
  }

  private max(metric: OracleMetric): number {
    return this.windows[metric].reduce((result, value) => Math.max(result, value), 0);
  }

  private countAtLeast(metric: OracleMetric, threshold: number): number {
    return this.windows[metric].filter((value) => value >= threshold).length;
  }

  evaluate(): XrPerformanceSnapshot {
    const fusionP95 = this.percentile('fusion', 0.95);
    const frameWorkP95 = this.percentile('frameWork', 0.95);
    const frameDeltaP95 = this.percentile('frameDelta', 0.95);
    const legacyCoverageP95 = this.percentile('coverage', 0.95);
    const coverageSnapshotP95 = this.windows.coverageSnapshot.length > 0
      ? this.percentile('coverageSnapshot', 0.95)
      : legacyCoverageP95;
    const coverageUploadP95 = this.percentile('coverageUpload', 0.95);
    const coverageDrawP95 = this.percentile('coverageDraw', 0.95);
    let pressure: XrPerformanceSnapshot['pressure'] = 'low';
    if (
      fusionP95 >= 90 || frameWorkP95 >= 32 || frameDeltaP95 >= 42 ||
      coverageSnapshotP95 >= 45 || coverageUploadP95 >= 24 || coverageDrawP95 >= 13
    ) pressure = 'high';
    else if (
      fusionP95 >= 55 || frameWorkP95 >= 22 || frameDeltaP95 >= 28 ||
      coverageSnapshotP95 >= 28 || coverageUploadP95 >= 13 || coverageDrawP95 >= 7
    ) pressure = 'medium';
    return {
      frameDeltaP50Ms: this.percentile('frameDelta', 0.50),
      frameDeltaP95Ms: frameDeltaP95,
      frameDeltaMaxMs: this.max('frameDelta'),
      frameWorkP95Ms: frameWorkP95,
      depthAccessP95Ms: this.percentile('depthAccess', 0.95),
      fusionP50Ms: this.percentile('fusion', 0.50),
      fusionP95Ms: fusionP95,
      fusionMaxMs: this.max('fusion'),
      coverageP95Ms: Math.max(legacyCoverageP95, coverageSnapshotP95, coverageUploadP95, coverageDrawP95),
      coverageSnapshotP95Ms: coverageSnapshotP95,
      coverageUploadP95Ms: coverageUploadP95,
      coverageDrawP95Ms: coverageDrawP95,
      longFrames33Ms: this.countAtLeast('frameDelta', 33),
      longFrames50Ms: this.countAtLeast('frameDelta', 50),
      sampleCount: Math.max(this.windows.frameDelta.length, this.windows.fusion.length),
      adaptiveCoverageIntervalMs: pressure === 'high' ? 1100 : pressure === 'medium' ? 780 : 550,
      adaptiveCoveragePointBudget: pressure === 'high' ? 4800 : pressure === 'medium' ? 8000 : 12000,
      adaptiveCoverageDrawIntervalMs: pressure === 'high' ? 32 : pressure === 'medium' ? 16 : 0,
      pressure,
    };
  }
}

interface StreamPhase {
  name: string;
  frames: number;
  frameMs: number;
  frameWork: number;
  draw: number;
  fusion: number;
  coverageSnapshot: number;
  coverageUpload: number;
  coverageLegacy: number;
  depthAccess: number;
  expected: { pressure: XrPerformanceSnapshot['pressure']; pointBudget: number; drawIntervalMs: number; coverageIntervalMs: number };
}

// Each phase runs long enough (400 frames, metrics every second frame => 200
// samples) to flush every 160-sample window, so its end is a steady state.
const STREAM_PHASES: readonly StreamPhase[] = [
  {
    name: 'calm', frames: 400, frameMs: 1000 / 60, frameWork: 10, draw: 2, fusion: 20,
    coverageSnapshot: 12, coverageUpload: 3, coverageLegacy: 15, depthAccess: 1,
    expected: { pressure: 'low', pointBudget: 12000, drawIntervalMs: 0, coverageIntervalMs: 550 },
  },
  {
    name: 'medium', frames: 400, frameMs: 30, frameWork: 24, draw: 8, fusion: 60,
    coverageSnapshot: 30, coverageUpload: 14, coverageLegacy: 40, depthAccess: 2,
    expected: { pressure: 'medium', pointBudget: 8000, drawIntervalMs: 16, coverageIntervalMs: 780 },
  },
  {
    name: 'high', frames: 400, frameMs: 46, frameWork: 34, draw: 14, fusion: 95,
    coverageSnapshot: 50, coverageUpload: 27, coverageLegacy: 60, depthAccess: 3,
    expected: { pressure: 'high', pointBudget: 4800, drawIntervalMs: 32, coverageIntervalMs: 1100 },
  },
  {
    name: 'recovered calm', frames: 400, frameMs: 1000 / 60, frameWork: 10, draw: 2, fusion: 20,
    coverageSnapshot: 12, coverageUpload: 3, coverageLegacy: 15, depthAccess: 1,
    expected: { pressure: 'low', pointBudget: 12000, drawIntervalMs: 0, coverageIntervalMs: 550 },
  },
];

/** Deterministic, RNG-free jitter in [0, span] derived from the frame index. */
const wobble = (frame: number, salt: number, span: number): number => (frame * 7 + salt * 13) % (span + 1);

describe('cached render policy (MAX-409)', () => {
  it('stays within the documented refresh interval of the reference math and converges to it exactly', () => {
    const profiler = new XrPerformanceProfiler();
    const oracle = new OldSnapshotMathOracle();
    const feed = (metric: XrPerfMetric, value: number): void => {
      profiler.record(metric, value);
      oracle.push(metric, value);
    };
    const boundaryStates: { timeMs: number; pointBudget: number; drawIntervalMs: number; coverageIntervalMs: number }[] = [];
    const mismatches: string[] = [];
    let timeMs = 0;
    let frame = 0;
    for (const phase of STREAM_PHASES) {
      for (let index = 0; index < phase.frames; index += 1, frame += 1) {
        if (frame > 0) timeMs += phase.frameMs;
        profiler.recordFrameTimestamp(timeMs);
        oracle.frame(timeMs);
        // Policy read at the frame boundary, where the cache may just have refreshed:
        // it must equal the reference state of some boundary within the interval.
        const reference = oracle.evaluate();
        boundaryStates.push({
          timeMs,
          pointBudget: reference.adaptiveCoveragePointBudget,
          drawIntervalMs: reference.adaptiveCoverageDrawIntervalMs,
          coverageIntervalMs: reference.adaptiveCoverageIntervalMs,
        });
        const cached = activeXrRenderPolicy();
        const cachedCoverageIntervalMs = profiler.recommendedCoverageIntervalMs();
        const explained = boundaryStates.some((state) => (
          timeMs - state.timeMs <= XR_POLICY_REFRESH_INTERVAL_MS &&
          state.pointBudget === cached.pointBudget &&
          state.drawIntervalMs === cached.drawIntervalMs &&
          state.coverageIntervalMs === cachedCoverageIntervalMs
        ));
        if (!explained) {
          mismatches.push(
            `${phase.name} frame ${index}: cached ${JSON.stringify(cached)}/${cachedCoverageIntervalMs} ms ` +
            `matches no reference state of the last ${XR_POLICY_REFRESH_INTERVAL_MS} ms`,
          );
        }
        feed('frameWork', phase.frameWork + wobble(frame, 1, 2));
        feed('coverageDraw', phase.draw + wobble(frame, 2, 1));
        if (frame % 2 === 0) {
          feed('fusion', phase.fusion + wobble(frame, 3, 6));
          feed('coverageSnapshot', phase.coverageSnapshot + wobble(frame, 4, 4));
          feed('coverageUpload', phase.coverageUpload + wobble(frame, 5, 2));
          feed('coverage', phase.coverageLegacy + wobble(frame, 6, 3));
          feed('depthAccess', phase.depthAccess + wobble(frame, 7, 1));
        }
      }
      expect(mismatches.slice(0, 3), `${phase.name}: cached policy lag`).toEqual([]);
      // Steady state at the end of the phase: exact parity with the reference math.
      expect(oracle.evaluate().pressure, `${phase.name}: reference pressure`).toBe(phase.expected.pressure);
      expect(activeXrRenderPolicy(), `${phase.name}: cached render policy`).toEqual({
        pointBudget: phase.expected.pointBudget,
        drawIntervalMs: phase.expected.drawIntervalMs,
      });
      expect(profiler.recommendedCoverageIntervalMs(), `${phase.name}: coverage interval`).toBe(phase.expected.coverageIntervalMs);
      expect(activeXrPerformancePressure(), `${phase.name}: scheduler pressure`).toBe(phase.expected.pressure);
      expect(profiler.snapshot(), `${phase.name}: full snapshot`).toEqual(oracle.evaluate());
    }
  });

  it('adopts the exact pressure whenever a full snapshot is taken', () => {
    const profiler = new XrPerformanceProfiler();
    // One sample short of the unclocked cadence: reads alone have not re-evaluated the windows.
    for (let index = 0; index < XR_POLICY_UNCLOCKED_REFRESH_SAMPLES - 1; index += 1) profiler.record('coverageDraw', 15);
    expect(activeXrRenderPolicy()).toEqual({ pointBudget: 12000, drawIntervalMs: 0 });
    expect(profiler.snapshot().pressure).toBe('high');
    expect(activeXrRenderPolicy()).toEqual({ pointBudget: 4800, drawIntervalMs: 32 });
  });

  it('falls back to a sample-count refresh when no frame clock is available', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < XR_POLICY_UNCLOCKED_REFRESH_SAMPLES; index += 1) profiler.record('coverageDraw', 15);
    expect(activeXrRenderPolicy()).toEqual({ pointBudget: 4800, drawIntervalMs: 32 });
    expect(activeXrPerformancePressure()).toBe('high');
  });

  it('refreshes at once when the XR frame clock moves backwards without a reset', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 20; index += 1) {
      profiler.recordFrameTimestamp(100000 + index * 16.67);
      profiler.record('frameWork', 10);
    }
    expect(activeXrRenderPolicy()).toEqual({ pointBudget: 12000, drawIntervalMs: 0 });
    // A new XR session restarts its timestamps near zero under heavy load.
    for (let index = 0; index < 30; index += 1) {
      profiler.recordFrameTimestamp(index * 46);
      profiler.record('frameWork', 40);
    }
    expect(activeXrRenderPolicy()).toEqual({ pointBudget: 4800, drawIntervalMs: 32 });
  });

  it('serves the recommended*() helpers and the scheduler pressure from the same cached policy', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 40; index += 1) {
      profiler.recordFrameTimestamp(index * 46);
      profiler.record('frameWork', 38);
      profiler.record('coverageDraw', 15);
    }
    expect(profiler.recommendedCoverageIntervalMs()).toBe(1100);
    expect(profiler.recommendedCoveragePointBudget()).toBe(4800);
    expect(profiler.recommendedCoverageDrawIntervalMs()).toBe(32);
    expect(activeXrPerformancePressure()).toBe('high');
    expect(activeXrRenderPolicy()).toBe(profiler.renderPolicy());
  });

  it('reset() drops the cached policy together with the windows', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 40; index += 1) {
      profiler.recordFrameTimestamp(index * 60);
      profiler.record('fusion', 120);
    }
    expect(activeXrPerformancePressure()).toBe('high');
    profiler.reset();
    expect(activeXrPerformancePressure()).toBe('low');
    expect(activeXrRenderPolicy()).toEqual({ pointBudget: 12000, drawIntervalMs: 0 });
    expect(profiler.recommendedCoverageIntervalMs()).toBe(550);
  });
});
