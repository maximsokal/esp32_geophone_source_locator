import { describe, expect, it } from 'vitest';

import { XrPerformanceProfiler } from '../../src/xr/xrPerformanceProfiler';

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
