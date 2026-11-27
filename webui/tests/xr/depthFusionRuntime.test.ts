import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DepthFusionFrameContext } from '../../src/xr/depthPointCloudV2';
import { EMPTY_STRUCTURAL_GUIDE, RuntimeFusion } from '../../src/xr/depthFusionRuntime';
import { createDepthFusionHandler } from '../../src/xr/depthFusionCore';
import { latestRgbGuidance, setLatestRgbGuidance } from '../../src/xr/rgbGuidanceStore';
import type { FusionWorkerRequest, FusionWorkerResponse } from '../../src/xr/depthFusionProtocol';
import { ARCORE_NOISE, DEFAULT_INTRINSICS, insideCornerScene, orbitPoses, renderDepth } from './support/syntheticDepthScene';

/*
 * The runtime-facing side of the depth fusion worker (Phase 2, stage 1): Finish drains and takes the snapshot under a bound, the asynchronous answers are
 * tied to the scan that asked for them, the worker is released between sessions and recreated for the next, and a worker that fails is reported.
 */
function frames(count: number) {
  const scene = insideCornerScene([0, 0, 0], 0.6);
  const poses = orbitPoses([0, 0, 0.15], 0.9, 35, 0, 330, count);
  return poses.map((pose, index) => ({ ...renderDepth(scene, pose, index + 1, ARCORE_NOISE, DEFAULT_INTRINSICS), pose, index }));
}
const context = (frame: ReturnType<typeof frames>[number]): DepthFusionFrameContext => ({ keyframeId: frame.index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: frame.pose.eye });

/** A worker that runs the REAL handler, but delivers its answers only when the test says so (so that "slow" and "hung" workers can be staged). */
class StagedWorker {
  readonly handler: ReturnType<typeof createDepthFusionHandler>;
  terminated = false;
  hold = false;
  private readonly held: Array<() => void> = [];
  private readonly listeners = new Set<(event: { data: FusionWorkerResponse }) => void>();
  private readonly errors = new Set<(event: { message: string }) => void>();
  constructor() { this.handler = createDepthFusionHandler((message) => { const deliver = (): void => { for (const l of this.listeners) l({ data: message }); }; if (this.hold) this.held.push(deliver); else deliver(); }); }
  addEventListener(type: string, listener: (event: never) => void): void { if (type === 'message') this.listeners.add(listener as never); if (type === 'error') this.errors.add(listener as never); }
  removeEventListener(type: string, listener: (event: never) => void): void { if (type === 'message') this.listeners.delete(listener as never); if (type === 'error') this.errors.delete(listener as never); }
  postMessage(message: FusionWorkerRequest): void { this.handler.handle(message); }
  terminate(): void { this.terminated = true; }
  release(): void { this.hold = false; while (this.held.length > 0) this.held.shift()!(); }
  crash(message: string): void { for (const l of this.errors) l({ message }); }
}

function setup(options: ConstructorParameters<typeof RuntimeFusion>[0] = {}) {
  const workers: StagedWorker[] = [];
  const fusion = new RuntimeFusion({ workerFactory: () => { const w = new StagedWorker(); workers.push(w); return w as unknown as Worker; }, ...options });
  return { fusion, workers, submit: (frame: ReturnType<typeof frames>[number]) => fusion.submitFrame(frame.depth, frame.view, context(frame), null, 1000 + frame.index) };
}

describe('RuntimeFusion', () => {
  afterEach(() => vi.useRealTimers());

  it('FINISH: the accepted frames are fused, then the snapshot of the worker-owned state is taken (bounded); nothing accepted is lost', async () => {
    const { fusion, workers, submit } = setup();
    fusion.reset();
    const input = frames(8);                                                           /* the whole queue: 3 in flight + 5 reserved for keyframes */
    workers[0].hold = true;                                                            /* the worker is behind: the frames sit in its queue */
    expect(input.map(submit).every((outcome) => outcome === 'accepted')).toBe(true);
    const finishing = fusion.finish(8, 8);
    workers[0].release();
    const result = await finishing;
    expect(result.drained).toBe(true);
    expect(result.outstandingKeyframes).toBe(0);
    expect(result.snapshot.pointCount).toBeGreaterThan(50);
    expect(fusion.stats.integratedFrames).toBe(8);
  });

  it('FINISH UNDER LOAD: a worker that falls behind makes the queue refuse frames, never grow, and Finish still ends with every accepted keyframe fused', async () => {
    const { fusion, workers, submit } = setup();
    fusion.reset();
    workers[0].hold = true;
    const outcomes = frames(40).map(submit);
    const accepted = outcomes.filter((o) => o === 'accepted').length;
    expect(accepted).toBe(8);                                                          /* 3 in flight + 5 reserved for keyframes */
    expect(outcomes.filter((o) => o === 'dropped-keyframe')).toHaveLength(32);
    expect(fusion.client.counters.maxQueueDepth).toBe(8);
    const finishing = fusion.finish(40, accepted);
    workers[0].release();
    const result = await finishing;
    expect(result.drained).toBe(true);
    expect(fusion.stats.integratedFrames).toBe(accepted);
    expect(result.snapshot.integrationKeyframes).toBe(accepted);
  });

  it('HUNG WORKER: Finish is bounded; it reports the failure instead of waiting for ever or inventing an empty scan', async () => {
    vi.useFakeTimers();
    const { fusion, workers, submit } = setup({ drainTimeoutMs: 500, requestTimeoutMs: 500 });
    fusion.reset();
    workers[0].hold = true;
    submit(frames(1)[0]);
    const outcome = fusion.finish(1, 1).then(() => 'resolved', (error: Error) => error.message);
    await vi.advanceTimersByTimeAsync(2_000);
    await expect(outcome).resolves.toMatch(/timed out/);
  });

  it('STALE: coverage / live-mesh / structural answers of a scan that has been reset are dropped', async () => {
    const { fusion, workers, submit } = setup();
    fusion.reset();
    for (const frame of frames(8)) submit(frame);
    workers[0].hold = true;
    fusion.requestCoverage(4000);
    fusion.requestLiveMeshInput(4000);
    fusion.reset();                                                                    /* a new scan */
    workers[0].release();
    await Promise.resolve();
    expect(fusion.takeCoverage()).toBeNull();
    expect(fusion.takeLiveMeshInput()).toBeNull();
  });

  it('STALE: an answer that arrived but was not yet handled when the scan was reset is dropped too', async () => {
    const { fusion, submit } = setup();
    fusion.reset();
    for (const frame of frames(8)) submit(frame);
    fusion.requestCoverage(4000);                                                      /* answered at once; its handler has not run yet */
    fusion.reset();
    for (let i = 0; i < 6; i += 1) await Promise.resolve();
    expect(fusion.takeCoverage()).toBeNull();
  });

  it('CPU FALLBACK: fusing inline leaves the runtime’s own RGB guidance frame where it was', () => {
    const fusion = new RuntimeFusion({ workerFactory: () => null });
    fusion.reset();
    const mine = { width: 1, height: 1, luma: new Uint8Array(1), edgeStrength: new Uint8Array(1), gradientX: new Int8Array(1), gradientY: new Int8Array(1), capturedAtMs: -1e9 };
    setLatestRgbGuidance(mine);
    try {
      const frame = frames(1)[0];
      fusion.submitFrame(frame.depth, frame.view, context(frame), null, 1000);
      expect(latestRgbGuidance()).toBe(mine);
    } finally { setLatestRgbGuidance(null); }
  });

  it('COVERAGE and LIVE MESH are asked for once at a time and delivered once', async () => {
    const { fusion, submit } = setup();
    fusion.reset();
    for (const frame of frames(12)) submit(frame);
    fusion.requestCoverage(4000); fusion.requestCoverage(4000);
    await Promise.resolve(); await Promise.resolve();
    const coverage = fusion.takeCoverage();
    expect(coverage).not.toBeNull();
    expect(coverage!.pointCount).toBeGreaterThan(0);
    expect(fusion.takeCoverage()).toBeNull();
    fusion.requestLiveMeshInput(4000);
    await Promise.resolve(); await Promise.resolve();
    const input = fusion.takeLiveMeshInput();
    expect(input === null || input.positions.length >= 16 * 3).toBe(true);
    expect(fusion.takeLiveMeshInput()).toBeNull();
  });

  it('SESSIONS: park releases the worker, the next reset starts a fresh one with an empty state', async () => {
    const { fusion, workers, submit } = setup();
    fusion.reset();
    for (const frame of frames(6)) submit(frame);
    await fusion.finish(6, 6);
    fusion.park();
    expect(workers[0].terminated).toBe(true);
    fusion.reset();
    expect(workers).toHaveLength(2);
    expect(fusion.stats.pointCount).toBe(0);
    expect(submit(frames(1)[0])).toBe('accepted');
  });

  it('CRASH: a worker error is reported to the owner and later answers fail instead of returning an empty scan', async () => {
    const onFailure = vi.fn();
    const { fusion, workers } = setup({ onFailure });
    fusion.reset();
    workers[0].crash('out of memory');
    expect(onFailure).toHaveBeenCalledWith('out of memory');
    await expect(fusion.finish(0, 0)).rejects.toThrow(/failed/);
  });

  it('PIN PREVIEWS: before the first certified snapshot arrives the placement sees an empty surface; afterwards the latest certified one (inline: always fresh)', async () => {
    const { fusion, submit } = setup();
    fusion.reset();
    expect(fusion.structuralGuideMirror()).toBeNull();
    for (const frame of frames(14)) submit(frame);
    const certified = await fusion.requestStructuralGuideSnapshot();
    expect(certified.positions.length).toBeGreaterThan(0);
    expect(fusion.structuralGuideMirror()).toBe(certified);
    expect(EMPTY_STRUCTURAL_GUIDE.positions.length).toBe(0);
  });

  it('CPU FALLBACK: without a Worker the same fusion runs inline, synchronously, and answers the same way', async () => {
    const fusion = new RuntimeFusion({ workerFactory: () => null });
    expect(fusion.mode).toBe('inline');
    fusion.reset();
    const input = frames(8);
    for (const frame of input) expect(fusion.submitFrame(frame.depth, frame.view, context(frame), null, 1000 + frame.index)).toBe('accepted');
    expect(fusion.stats.integratedFrames).toBe(8);
    expect(fusion.inlineSnapshot(8, 8)!.pointCount).toBeGreaterThan(50);
    expect(fusion.structuralGuideMirror()!.positions.length).toBeGreaterThan(0);
  });
});
