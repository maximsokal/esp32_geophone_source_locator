import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { createDepthFusionHandler } from '../../src/xr/depthFusionCore';
import type { FusionWorkerRequest, FusionWorkerResponse } from '../../src/xr/depthFusionProtocol';
import { RuntimeFusion } from '../../src/xr/depthFusionRuntime';
import { classifyFusionFrame, FUSION_BASE_INTERVAL_MS, FusionCaptureGovernor, slotsFor } from '../../src/xr/fusionCaptureGovernor';
import { WebXrFusionKeyframeSelector, type FusionPoseLike } from '../../src/xr/webxrFusionQuality';
import { ARCORE_NOISE, DEFAULT_INTRINSICS, insideCornerScene, orbitPoses, renderDepth, worldFromView, type CameraPose, type SyntheticScene } from './support/syntheticDepthScene';

/*
 * Admission control before capture (Phase 2, stage 1). A virtual clock drives a camera at 5-60 Hz around a synthetic corner while the fusion "worker" (the real
 * handler and accumulator) takes a PHONE-like time per frame (75 ms; the phone log of 2026-11-26 measured 50-90 ms p50), slowed 1x to 8x, with stalls.
 * What must hold: nothing accepted is lost, nothing captured is refused afterwards, the queue stays bounded, and the SURFACE COVERAGE of the finished scan
 * stays close to that of an infinitely fast device seeing the same motion.
 */
const PHONE_FRAME_MS = 75;
const DURATION_MS = 24_000;
const ORBIT_DEG_PER_S = 13;
const TOLERANCE = 0.03;

/** A worker that finishes a frame only when the virtual clock reaches the end of its service time (one frame at a time, in order). */
class SimulatedWorker {
  private queue: Array<{ message: FusionWorkerRequest; service: number }> = [];
  private busyUntil = 0;
  private current = 0;
  now = 0;
  private readonly listeners = new Set<(event: { data: FusionWorkerResponse }) => void>();
  private readonly handler = createDepthFusionHandler((response) => {
    const timed = response.type === 'frameDone' ? { ...response, workMs: this.current } : response;
    for (const listener of this.listeners) listener({ data: timed });
  });
  constructor(private readonly serviceMs: (now: number) => number) {}
  addEventListener(type: string, listener: (event: never) => void): void { if (type === 'message') this.listeners.add(listener as never); }
  removeEventListener(type: string, listener: (event: never) => void): void { if (type === 'message') this.listeners.delete(listener as never); }
  terminate(): void { this.listeners.clear(); }
  postMessage(message: FusionWorkerRequest): void {
    if (message.type !== 'frame' && message.type !== 'request') { this.handler.handle(message); return; }
    if (this.queue.length === 0) this.busyUntil = Math.max(this.busyUntil, this.now);
    this.queue.push({ message, service: message.type === 'frame' ? this.serviceMs(this.now) : 0 });
    this.advance(this.now);
  }
  advance(now: number): void {
    this.now = now;
    while (this.queue.length > 0 && this.busyUntil + this.queue[0].service <= now) {
      const item = this.queue.shift()!;
      this.busyUntil += item.service;
      this.current = item.service;
      this.handler.handle(item.message);
    }
  }
}

function poseAt(timeMs: number, degPerSecond: number): CameraPose {
  const azimuth = (timeMs / 1000) * degPerSecond;
  return orbitPoses([0, 0, 0.15], 0.9, 35, azimuth, azimuth, 1)[0];
}

function fusionPose(pose: CameraPose): FusionPoseLike {
  const m = worldFromView(pose);
  const [m00, m10, m20, m01, m11, m21, m02, m12, m22] = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]];
  const trace = m00 + m11 + m22;
  let x: number; let y: number; let z: number; let w: number;
  if (trace > 0) { const s = Math.sqrt(trace + 1) * 2; w = s / 4; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s; }
  else if (m00 > m11 && m00 > m22) { const s = Math.sqrt(1 + m00 - m11 - m22) * 2; w = (m21 - m12) / s; x = s / 4; y = (m01 + m10) / s; z = (m02 + m20) / s; }
  else if (m11 > m22) { const s = Math.sqrt(1 + m11 - m00 - m22) * 2; w = (m02 - m20) / s; x = (m01 + m10) / s; y = s / 4; z = (m12 + m21) / s; }
  else { const s = Math.sqrt(1 + m22 - m00 - m11) * 2; w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = s / 4; }
  return { position: { x: m[12], y: m[13], z: m[14] }, orientation: { x, y, z, w } };
}

function sceneTargets(scene: SyntheticScene): number[][] {
  const out: number[][] = [];
  for (const polygon of scene.polygons) {
    const [a, b, c] = polygon.vertices;
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]]; const v = [c[0] - b[0], c[1] - b[1], c[2] - b[2]];
    const nu = Math.max(1, Math.ceil(Math.hypot(u[0], u[1], u[2]) / 0.02)); const nv = Math.max(1, Math.ceil(Math.hypot(v[0], v[1], v[2]) / 0.02));
    for (let i = 0; i <= nu; i += 1) for (let j = 0; j <= nv; j += 1) out.push([0, 1, 2].map((k) => a[k] + u[k] * (i / nu) + v[k] * (j / nv)));
  }
  return out;
}

function coverageOf(scene: SyntheticScene, positions: Float32Array): number {
  const targets = sceneTargets(scene);
  let hit = 0;
  for (const t of targets) {
    for (let i = 0; i < positions.length; i += 3) if (Math.abs(positions[i] - t[0]) <= TOLERANCE && Math.hypot(positions[i] - t[0], positions[i + 1] - t[1], positions[i + 2] - t[2]) <= TOLERANCE) { hit += 1; break; }
  }
  return hit / targets.length;
}

interface RunResult {
  capturedKeyframes: number; captured: number; deferredKeyframeAttempts: number; deferredOther: number; noSlotFrames: number; rejectedAfterCapture: number; lost: number;
  integrated: number; maxQueue: number; coverage: number; points: number; promoted: number; catchingUpEntries: number; finalIntervalMs: number;
}

async function scan(rateHz: number, serviceMs: (now: number) => number, degPerSecond = ORBIT_DEG_PER_S, durationMs = DURATION_MS): Promise<RunResult> {
  const scene = insideCornerScene([0, 0, 0], 0.6);
  const worker = new SimulatedWorker(serviceMs);
  const fusion = new RuntimeFusion({ workerFactory: () => worker as unknown as Worker, keeperFactory: () => null, journalLimit: 8 });
  fusion.reset();
  const selector = new WebXrFusionKeyframeSelector();
  let accepted = 0; let capturedKeyframes = 0; let rendered = 0;
  const period = 1000 / rateHz;
  for (let time = 0; time <= durationMs; time += period) {
    worker.advance(time);
    const pose = poseAt(time, degPerSecond);
    const step = fusion.captureStep(time, fusionPose(pose), selector);
    if (step.action !== 'capture') continue;
    rendered += 1;
    const frame = renderDepth(scene, pose, rendered, ARCORE_NOISE, DEFAULT_INTRINSICS);
    const keyframe = step.decision.kind === 'keyframe';
    const outcome = fusion.submitFrame(frame.depth, frame.view, { keyframeId: step.decision.keyframeId, evidenceKeyframe: keyframe, frameWeight: step.decision.frameWeight, cameraProjectPosition: pose.eye }, null, time);
    if (outcome === 'accepted') { accepted += 1; if (keyframe) capturedKeyframes += 1; }
  }
  const finishing = fusion.finish(rendered, accepted);
  worker.advance(durationMs + 1_000_000);                                               /* Finish: the accepted frames are all fused */
  await Promise.resolve(); await Promise.resolve();
  worker.advance(durationMs + 2_000_000);
  const result = await finishing;
  const c = fusion.client.counters; const g = fusion.governor.counters;
  const out: RunResult = {
    capturedKeyframes, captured: accepted, deferredKeyframeAttempts: g.deferred.keyframe, deferredOther: g.deferred.depth + g.deferred.polish + g.deferred.repeat, noSlotFrames: g.deferredNoSlot,
    rejectedAfterCapture: c.droppedKeyframes + c.droppedOptional, lost: c.lostFrames, integrated: fusion.stats.integratedFrames, maxQueue: c.maxQueueDepth,
    coverage: coverageOf(scene, result.snapshot.positions), points: result.snapshot.pointCount, promoted: fusion.stats.promotedCount, catchingUpEntries: g.catchingUpEntries, finalIntervalMs: Math.round(fusion.governor.intervalMs),
  };
  fusion.dispose();
  return out;
}

describe('frame classes and slots', () => {
  const decision = (kind: 'keyframe' | 'polish', state: string, translation = 0, rotation = 0) => ({ kind, state, translationFromKeyframeMeters: translation, rotationFromKeyframeRadians: rotation }) as never;
  it('ranks an informative viewpoint above ordinary depth, polish and a repeated view', () => {
    expect(classifyFusionFrame(decision('keyframe', 'keyframe'))).toBe('keyframe');
    expect(classifyFusionFrame(decision('polish', 'polish', 0.03))).toBe('depth');
    expect(classifyFusionFrame(decision('polish', 'polish', 0.01))).toBe('polish');
    expect(classifyFusionFrame(decision('polish', 'stationary'))).toBe('repeat');
    const queue = { queueDepth: 0, maxInFlight: 3, mandatoryReserve: 5 };
    expect((['keyframe', 'depth', 'polish', 'repeat'] as const).map((c) => slotsFor(c, queue))).toEqual([8, 3, 2, 1]);
  });

  it('with no free slot the selector is not even asked: the next keyframe id is not spent, and the operator is told to wait until the queue drains', () => {
    const governor = new FusionCaptureGovernor();
    const selector = new WebXrFusionKeyframeSelector();
    const full = { queueDepth: 8, maxInFlight: 3, mandatoryReserve: 5 };
    const free = { ...full, queueDepth: 0 };
    expect(governor.step(0, fusionPose(poseAt(0, 13)), selector, free).action).toBe('capture');
    expect(governor.step(1200, fusionPose(poseAt(1200, 13)), selector, full).action).toBe('no-slot');
    expect(governor.catchingUp).toBe(true);
    const next = governor.step(1216, fusionPose(poseAt(1200, 13)), selector, free);
    expect(next.action).toBe('capture');
    if (next.action === 'capture') expect([next.frameClass, next.decision.keyframeId]).toEqual(['keyframe', 2]);
    governor.frameDone(20, free);
    expect(governor.catchingUp).toBe(false);
  });

  it('a DEFERRED frame is not taken: it is captured as soon as its class has a slot, without waiting for the selector to offer a new one', () => {
    const governor = new FusionCaptureGovernor();
    const selector = new WebXrFusionKeyframeSelector();
    const free = { queueDepth: 0, maxInFlight: 3, mandatoryReserve: 5 };
    const still = fusionPose(poseAt(0, 13));
    expect(governor.step(0, still, selector, free).action).toBe('capture');
    const deferred = governor.step(2000, still, selector, { ...free, queueDepth: 1 });   /* a repeat may only run while the worker is idle */
    expect(deferred).toMatchObject({ action: 'deferred', frameClass: 'repeat' });
    expect(governor.counters.deferred.repeat).toBe(1);
    expect(governor.step(2016, still, selector, free), 'the very next XR frame').toMatchObject({ action: 'capture', frameClass: 'repeat' });
  });

  it('a frame the fusion REFUSED after admission is handed back: the same viewpoint is offered again as the same keyframe', () => {
    const governor = new FusionCaptureGovernor();
    const selector = new WebXrFusionKeyframeSelector();
    const free = { queueDepth: 0, maxInFlight: 3, mandatoryReserve: 5 };
    expect(governor.step(0, fusionPose(poseAt(0, 13)), selector, free).action).toBe('capture');
    const moved = fusionPose(poseAt(1200, 13));
    const first = governor.step(1200, moved, selector, free);
    expect(first).toMatchObject({ action: 'capture', frameClass: 'keyframe' });
    if (first.action !== 'capture') return;
    expect(first.decision.keyframeId).toBe(2);
    governor.refuse(first, selector);
    expect(governor.counters.captured.keyframe, 'it does not count as captured').toBe(1);
    const again = governor.step(1400, moved, selector, free);
    expect(again).toMatchObject({ action: 'capture', frameClass: 'keyframe' });
    if (again.action === 'capture') expect(again.decision.keyframeId).toBe(2);
  });

  it('the capture interval follows the measured fusion time with hysteresis: wider at once, narrower only after a run of short queues', () => {
    const governor = new FusionCaptureGovernor();
    const queue = (queueDepth: number) => ({ queueDepth, maxInFlight: 3, mandatoryReserve: 5 });
    expect(governor.intervalMs).toBe(FUSION_BASE_INTERVAL_MS);
    for (let i = 0; i < 12; i += 1) governor.frameDone(400, queue(3));
    const widened = governor.intervalMs;
    expect(widened).toBeGreaterThanOrEqual(480);                                         /* at least the measured time with headroom */
    for (let i = 0; i < 5; i += 1) governor.frameDone(20, queue(0));
    expect(governor.intervalMs, 'five frames with a short queue change nothing').toBe(widened);
    governor.frameDone(20, queue(2));
    for (let i = 0; i < 5; i += 1) governor.frameDone(20, queue(0));
    expect(governor.intervalMs, 'a longer queue in between starts the run again').toBe(widened);
    governor.frameDone(20, queue(0));
    expect(governor.intervalMs, 'the sixth in a row narrows it by one step').toBeCloseTo(widened * 0.85, 6);
    for (let i = 0; i < 120; i += 1) governor.frameDone(20, queue(0));
    expect(governor.intervalMs).toBe(FUSION_BASE_INTERVAL_MS);
    governor.frameDone(20, queue(3));
    expect(governor.intervalMs, 'a full queue widens it at once').toBeGreaterThan(FUSION_BASE_INTERVAL_MS);
  });
});

describe('the measured fusion time is a floor of its own, not only the queue', () => {
  it('a slow worker whose queue is short again (because the interval widened) is not sped back up past what it can fuse', () => {
    const governor = new FusionCaptureGovernor();
    const idle = { queueDepth: 0, maxInFlight: 3, mandatoryReserve: 5 };
    for (let i = 0; i < 80; i += 1) governor.frameDone(600, idle);
    expect(governor.intervalMs, '600 ms per frame with 20 % headroom').toBeGreaterThanOrEqual(720);
    for (let i = 0; i < 80; i += 1) governor.frameDone(20, idle);
    expect(governor.intervalMs, 'and it comes back once the worker is fast again').toBe(FUSION_BASE_INTERVAL_MS);
  });
});

describe('a slow worker that still has spare capacity loses no viewpoint (desktop replay 2026-11-27: +200 ms per frame cost 26 of 101 keyframes)', () => {
  it('at 30 Hz with 200 and 250 ms per frame, informative keyframes are captured as often as on an ideal device', async () => {
    const ideal = await scan(30, () => 0);
    for (const serviceMs of [200, 250]) {
      const slow = await scan(30, () => serviceMs);
      expect(ideal.capturedKeyframes * serviceMs, 'precondition: the worker has time for every keyframe').toBeLessThan(DURATION_MS * 0.9);
      expect(slow.capturedKeyframes, `${serviceMs} ms per frame`).toBeGreaterThanOrEqual(Math.floor(ideal.capturedKeyframes * 0.97));
      expect(slow.coverage, `${serviceMs} ms per frame: coverage within 1 point`).toBeGreaterThanOrEqual(ideal.coverage - 0.01);
      expect(slow.lost + slow.rejectedAfterCapture).toBe(0);
    }
  }, 300_000);

  it('only keyframes get ahead of the measured throughput: a frame that adds no new viewpoint waits for the measured pace of the worker', () => {
    const governor = new FusionCaptureGovernor();
    const selector = new WebXrFusionKeyframeSelector();
    const idle = { queueDepth: 0, maxInFlight: 3, mandatoryReserve: 5 };
    for (let i = 0; i < 12; i += 1) governor.frameDone(500, idle);
    expect(governor.intervalMs).toBeGreaterThanOrEqual(600);
    const still = fusionPose(poseAt(0, 13));
    expect(governor.step(10_000, still, selector, idle).action).toBe('capture');
    for (let time = 10_016; time < 10_000 + governor.intervalMs; time += 16) expect(governor.step(time, still, selector, idle).action, `a repeated view at +${time - 10_000} ms`).not.toBe('capture');
    const later = 10_000 + governor.intervalMs + 400;
    expect(governor.step(later, still, selector, idle), 'and is taken once the measured interval has passed').toMatchObject({ action: 'capture', frameClass: 'repeat' });
    expect(governor.intervalMs, 'the next frame comes well inside the measured interval').toBeGreaterThan(300);
    expect(governor.step(later + 300, fusionPose(poseAt(1200, 13)), selector, idle), 'a new viewpoint does not wait for it').toMatchObject({ action: 'capture', frameClass: 'keyframe' });
  });

  it('with a backlog the measured throughput holds back keyframes too, so the queue (and the delay to the mesh and to Finish) stays short', () => {
    const governor = new FusionCaptureGovernor();
    const selector = new WebXrFusionKeyframeSelector();
    const backlog = { queueDepth: 3, maxInFlight: 3, mandatoryReserve: 5 };
    for (let i = 0; i < 12; i += 1) governor.frameDone(500, backlog);
    expect(governor.step(10_000, fusionPose(poseAt(0, 13)), selector, backlog).action).toBe('capture');
    expect(governor.step(10_300, fusionPose(poseAt(1200, 13)), selector, backlog).action).toBe('not-due');
    expect(governor.step(10_300, fusionPose(poseAt(1200, 13)), selector, { ...backlog, queueDepth: 2 })).toMatchObject({ action: 'capture', frameClass: 'keyframe' });
  });
});

describe('capture under load: camera 5-60 Hz, fusion slowed 1x-8x from a phone-like 75 ms per frame', () => {
  const table: Record<string, RunResult> = {};
  const reference = new Map<number, Promise<RunResult>>();
  const ideal = (rate: number): Promise<RunResult> => { let value = reference.get(rate); if (value === undefined) { value = scan(rate, () => 0); reference.set(rate, value); } return value; };

  for (const rate of [5, 10, 15, 30, 60]) {
    for (const slow of [1, 2, 4, 8]) {
      it(`${rate} Hz, ${slow}x: nothing accepted is lost or refused afterwards, the queue stays bounded, coverage stays within 5 points of an ideal device`, async () => {
        const best = await ideal(rate);
        const run = await scan(rate, () => PHONE_FRAME_MS * slow);
        table[`${rate}Hz-${slow}x`] = run;
        table[`${rate}Hz-ideal`] = best;
        expect(run.lost).toBe(0);
        expect(run.rejectedAfterCapture).toBe(0);
        expect(run.integrated).toBe(run.captured);
        expect(run.maxQueue).toBeLessThanOrEqual(8);
        expect(run.coverage, `coverage ${run.coverage.toFixed(3)} vs ideal ${best.coverage.toFixed(3)}`).toBeGreaterThanOrEqual(best.coverage - 0.05);
      }, 300_000);
    }
  }

  it('BURSTS: 2.5 s stalls every 6 s (thermal throttling) lose nothing and keep coverage', async () => {
    const best = await ideal(30);
    const run = await scan(30, (now) => (now % 6000 < 2500 ? 700 : PHONE_FRAME_MS));
    table['30Hz-burst'] = run;
    expect(run.lost).toBe(0);
    expect(run.rejectedAfterCapture).toBe(0);
    expect(run.integrated).toBe(run.captured);
    expect(run.maxQueue).toBeLessThanOrEqual(8);
    expect(run.coverage).toBeGreaterThanOrEqual(best.coverage - 0.05);
    const out = resolve(__dirname, '../../../.test-build/phase2/governor-simulation.json');
    mkdirSync(resolve(out, '..'), { recursive: true });
    writeFileSync(out, JSON.stringify({ phoneFrameMs: PHONE_FRAME_MS, durationMs: DURATION_MS, orbitDegPerS: ORBIT_DEG_PER_S, runs: table }, null, 2));
  }, 300_000);

  it('a STATIONARY camera (1000 camera frames of one view) earns no confidence from repeats: the confirmed surface does not grow after the first seconds', async () => {
    const early = await scan(30, () => PHONE_FRAME_MS, 0, 4_000);
    const long = await scan(30, () => PHONE_FRAME_MS, 0, 33_400);
    expect(long.captured).toBeGreaterThan(early.captured * 3);
    expect(long.capturedKeyframes, 'one viewpoint is one keyframe').toBe(1);
    expect(long.promoted).toBe(early.promoted);
    expect(long.coverage).toBeLessThanOrEqual(early.coverage + 0.02);
  }, 300_000);
});
