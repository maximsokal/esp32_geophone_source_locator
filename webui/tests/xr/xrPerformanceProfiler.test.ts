import { describe, expect, it } from 'vitest';

import { XrPerformanceProfiler } from '../../src/xr/xrPerformanceProfiler';

describe('XrPerformanceProfiler', () => {
  it('keeps frame timing bounded and reports percentiles', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 220; index += 1) {
      profiler.recordFrameTimestamp(index * 16.67);
      profiler.record('fusion', 18 + (index % 7));
      profiler.record('coverage', 8 + (index % 5));
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.sampleCount).toBeLessThanOrEqual(160);
    expect(snapshot.frameDeltaP95Ms).toBeGreaterThan(16);
    expect(snapshot.frameDeltaP95Ms).toBeLessThan(18);
    expect(snapshot.pressure).toBe('low');
    expect(snapshot.adaptiveCoverageIntervalMs).toBe(550);
  });

  it('raises coverage interval under medium pressure', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 80; index += 1) {
      profiler.recordFrameTimestamp(index * 30);
      profiler.record('fusion', 62);
      profiler.record('coverage', 31);
      profiler.record('frameWork', 24);
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.pressure).toBe('medium');
    expect(snapshot.adaptiveCoverageIntervalMs).toBe(780);
  });

  it('raises coverage interval aggressively under high pressure', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 80; index += 1) {
      profiler.recordFrameTimestamp(index * 46);
      profiler.record('fusion', 105);
      profiler.record('coverage', 55);
      profiler.record('frameWork', 38);
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.pressure).toBe('high');
    expect(snapshot.adaptiveCoverageIntervalMs).toBe(1100);
    expect(snapshot.longFrames33Ms).toBeGreaterThan(0);
    expect(snapshot.longFrames50Ms).toBe(0);
  });

  it('reset clears historical pressure', () => {
    const profiler = new XrPerformanceProfiler();
    for (let index = 0; index < 20; index += 1) {
      profiler.recordFrameTimestamp(index * 60);
      profiler.record('fusion', 120);
    }
    expect(profiler.snapshot().pressure).toBe('high');
    profiler.reset();
    expect(profiler.snapshot().pressure).toBe('low');
    expect(profiler.snapshot().sampleCount).toBe(0);
  });
});
