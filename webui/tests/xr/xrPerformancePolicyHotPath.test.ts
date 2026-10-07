import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  LiveCoveragePointRenderer,
  type CoverageRenderLayer,
  type CoverageRenderSnapshot,
  type CoverageRenderView,
} from '../../src/xr/liveCoverageRenderer';
import {
  XR_POLICY_REFRESH_INTERVAL_MS,
  XrPerformanceProfiler,
  activeXrPerformancePressure,
  activeXrRenderPolicy,
} from '../../src/xr/xrPerformanceProfiler';

/*
 * MAX-409: the render policy is read on every visible XR frame, so a read must
 * not evaluate (sort) the rolling percentile windows. Array.prototype.sort is
 * spied on directly: RollingSamples.percentile is the only sorting call site in
 * the profiler, the renderer and the policy helpers, and the simulations below
 * never sort anything themselves between the spy and its readout.
 */

const FRAME_MS = 1000 / 60;
const FRAMES = 5 * 60;
// Pressure depends on six rolling windows (fusion, frameWork, frameDelta,
// coverage snapshot, upload, draw); one refresh sorts each non-empty one once.
const MAX_SORTS_PER_POLICY_REFRESH = 6;
const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
const VIEWS: readonly CoverageRenderView[] = [{ projectionMatrix: IDENTITY, transform: { matrix: IDENTITY } }];
const LAYER: CoverageRenderLayer = {
  framebuffer: null,
  getViewport: () => ({ x: 0, y: 0, width: 640, height: 480 }),
};

function coverageSnapshot(pointCount: number): CoverageRenderSnapshot {
  const positions = new Float32Array(pointCount * 3);
  const confidence = new Float32Array(pointCount);
  for (let index = 0; index < pointCount; index += 1) {
    positions[index * 3] = index;
    positions[index * 3 + 1] = index + 0.25;
    positions[index * 3 + 2] = index + 0.5;
    confidence[index] = 0.2 + (index % 7) * 0.1;
  }
  return { positions, confidence, pointCount, manualPointCount: 0 };
}

/** Minimal GL double: enough for the real LiveCoveragePointRenderer to build, upload and draw. */
function createFakeGl(onDraw: () => void = () => undefined) {
  const uploadedLengths: number[] = [];
  const noop = (): void => undefined;
  const gl = {
    VERTEX_SHADER: 0x8b31, FRAGMENT_SHADER: 0x8b30, COMPILE_STATUS: 0x8b81, LINK_STATUS: 0x8b82,
    ARRAY_BUFFER: 0x8892, DYNAMIC_DRAW: 0x88e8, FLOAT: 0x1406, POINTS: 0, BLEND: 0x0be2,
    SRC_ALPHA: 0x0302, ONE_MINUS_SRC_ALPHA: 0x0303, DEPTH_TEST: 0x0b71, LEQUAL: 0x0203,
    createShader: () => ({}), shaderSource: noop, compileShader: noop,
    getShaderParameter: () => true, getShaderInfoLog: () => null, deleteShader: noop,
    createProgram: () => ({}), attachShader: noop, linkProgram: noop,
    getProgramParameter: () => true, getProgramInfoLog: () => null, deleteProgram: noop,
    createBuffer: () => ({}), deleteBuffer: noop, getAttribLocation: () => 0, getUniformLocation: () => ({}),
    bindBuffer: noop,
    bufferData: (_target: number, data: Float32Array) => { uploadedLengths.push(data.length); },
    useProgram: noop, enable: noop, disable: noop, blendFunc: noop, depthFunc: noop, depthMask: noop,
    enableVertexAttribArray: noop, vertexAttribPointer: noop, disableVertexAttribArray: noop,
    viewport: noop, uniformMatrix4fv: noop,
    drawArrays: () => onDraw(),
  };
  return { gl: gl as unknown as WebGLRenderingContext, uploadedLengths };
}

/**
 * Five simulated seconds of 60 Hz coverage rendering, shaped like the XR frame
 * loop: frame boundary, frame work, per-frame render() policy read, the
 * per-depth-frame recommended-interval read, the scheduler's pressure read and
 * a coverage upload about every half second. Sorts are attributed to policy
 * readers or to explicit snapshot() calls; anything else is a cache refresh.
 */
function simulateCoverageFrames(options: { snapshotEveryFrames?: number } = {}) {
  const sortSpy = vi.spyOn(Array.prototype, 'sort');
  const profiler = new XrPerformanceProfiler();
  const { gl } = createFakeGl();
  const renderer = new LiveCoveragePointRenderer(gl);
  const coverage = coverageSnapshot(3000);
  renderer.update(coverage);
  let sortsInsidePolicyReaders = 0;
  let sortsInsideSnapshots = 0;
  const countSorts = (run: () => void): number => {
    const before = sortSpy.mock.calls.length;
    run();
    return sortSpy.mock.calls.length - before;
  };
  for (let frame = 0; frame < FRAMES; frame += 1) {
    profiler.recordFrameTimestamp(frame * FRAME_MS);
    profiler.record('frameWork', 12 + (frame % 5));
    if (frame % 2 === 0) {
      profiler.record('fusion', 20 + (frame % 7));
      profiler.record('depthAccess', 2);
    }
    if (frame % 30 === 0) {
      profiler.record('coverage', 8);
      profiler.record('coverageSnapshot', 6);
      sortsInsidePolicyReaders += countSorts(() => renderer.update(coverage));
    }
    sortsInsidePolicyReaders += countSorts(() => renderer.render(LAYER, VIEWS));
    sortsInsidePolicyReaders += countSorts(() => activeXrRenderPolicy());
    sortsInsidePolicyReaders += countSorts(() => profiler.recommendedCoverageIntervalMs());
    sortsInsidePolicyReaders += countSorts(() => activeXrPerformancePressure());
    if (options.snapshotEveryFrames !== undefined && frame % options.snapshotEveryFrames === 0) {
      sortsInsideSnapshots += countSorts(() => profiler.snapshot());
    }
  }
  return { sortsInsidePolicyReaders, sortsInsideSnapshots, totalSorts: sortSpy.mock.calls.length };
}

describe('XR render policy hot path (MAX-409)', () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('never sorts a rolling window when the renderer, scheduler or recommendation helpers read the policy', () => {
    const run = simulateCoverageFrames();
    expect(run.sortsInsidePolicyReaders).toBe(0);
  });

  it('re-evaluates the rolling windows only at the documented refresh cadence', () => {
    const run = simulateCoverageFrames();
    const durationMs = FRAMES * FRAME_MS;
    const maxRefreshes = Math.floor(durationMs / XR_POLICY_REFRESH_INTERVAL_MS) + 1;
    // A refresh is due once per interval plus at most one frame period; every
    // refresh sorts at least the always-populated frame-delta window.
    const minRefreshes = Math.floor((durationMs - FRAME_MS) / (XR_POLICY_REFRESH_INTERVAL_MS + FRAME_MS));
    expect(run.totalSorts).toBeGreaterThanOrEqual(minRefreshes);
    expect(run.totalSorts).toBeLessThanOrEqual(maxRefreshes * MAX_SORTS_PER_POLICY_REFRESH);
  });

  it('runs full snapshots only when asked and lets a 10 Hz snapshot cadence satisfy the cache refresh', () => {
    const run = simulateCoverageFrames({ snapshotEveryFrames: 6 });
    expect(run.sortsInsidePolicyReaders).toBe(0);
    expect(run.sortsInsideSnapshots).toBeGreaterThan(0);
    // snapshot() evaluates the very same windows, so with publishes every 100 ms
    // (< XR_POLICY_REFRESH_INTERVAL_MS) the cadence refresh never has to run.
    expect(run.totalSorts).toBe(run.sortsInsideSnapshots);
  });

  /*
   * Self-observer regression. The fake clock advances by the simulated workload
   * plus a deliberately pessimistic SORT_COST_MS per Array.prototype.sort, so
   * any policy bookkeeping that runs inside a measured window shows up in the
   * profiler's own frameWork / coverageDraw samples. The assertion is about where
   * bookkeeping may run (outside measured windows), not about absolute speed.
   * Exact sums: every value is a multiple of 0.5 ms, so the p95 values are exact.
   */
  const SORT_COST_MS = 0.5;
  it.each([
    {
      name: 'a workload just below the medium gates',
      frameMs: 25, frameWorkMs: 21.5, drawMs: 6.5,
      pressure: 'low', policy: { pointBudget: 12000, drawIntervalMs: 0 },
    },
    {
      name: 'a workload just below the high gates',
      frameMs: 40, frameWorkMs: 31, drawMs: 12.5,
      pressure: 'medium', policy: { pointBudget: 8000, drawIntervalMs: 16 },
    },
  ])('does not measure its own policy bookkeeping: $name stays at $pressure pressure', (testCase) => {
    const sortSpy = vi.spyOn(Array.prototype, 'sort');
    let frameBaseMs = 0;
    let intraFrameMs = 0;
    let sortsAtFrameStart = 0;
    const clock = (): number => (
      frameBaseMs + intraFrameMs + (sortSpy.mock.calls.length - sortsAtFrameStart) * SORT_COST_MS
    );
    vi.spyOn(performance, 'now').mockImplementation(clock);
    const profiler = new XrPerformanceProfiler();
    const { gl } = createFakeGl(() => { intraFrameMs += testCase.drawMs; });
    const renderer = new LiveCoveragePointRenderer(gl);
    renderer.update(coverageSnapshot(3000));
    for (let frame = 0; frame < 200; frame += 1) {
      frameBaseMs = frame * testCase.frameMs;
      intraFrameMs = 0;
      sortsAtFrameStart = sortSpy.mock.calls.length;
      profiler.recordFrameTimestamp(frameBaseMs);
      const frameStarted = performance.now();
      intraFrameMs += testCase.frameWorkMs - testCase.drawMs;
      renderer.render(LAYER, VIEWS);
      profiler.recommendedCoverageIntervalMs();
      profiler.record('frameWork', performance.now() - frameStarted);
    }
    const snapshot = profiler.snapshot();
    expect(snapshot.frameWorkP95Ms).toBe(testCase.frameWorkMs);
    expect(snapshot.coverageDrawP95Ms).toBe(testCase.drawMs);
    expect(snapshot.pressure).toBe(testCase.pressure);
    expect(activeXrRenderPolicy()).toEqual(testCase.policy);
  });

  it('feeds the cached policy point budget into the live render upload once pressure is refreshed', () => {
    const profiler = new XrPerformanceProfiler();
    const { gl, uploadedLengths } = createFakeGl();
    const renderer = new LiveCoveragePointRenderer(gl);
    const coverage = coverageSnapshot(20000);
    renderer.update(coverage);
    expect(uploadedLengths.slice(-2)).toEqual([12000 * 3, 12000]);
    for (let frame = 0; frame < 40; frame += 1) {
      profiler.recordFrameTimestamp(frame * 46);
      profiler.record('frameWork', 40);
    }
    renderer.update(coverage);
    expect(uploadedLengths.slice(-2)).toEqual([4800 * 3, 4800]);
  });

  it('keeps the refresh interval short enough for a meaningful pressure response', () => {
    // 250 ms is about 15 frames at 60 Hz; a much longer interval would make the
    // adaptive policy too sluggish to protect the frame loop.
    expect(XR_POLICY_REFRESH_INTERVAL_MS).toBeGreaterThan(0);
    expect(XR_POLICY_REFRESH_INTERVAL_MS).toBeLessThanOrEqual(250);
  });
});
