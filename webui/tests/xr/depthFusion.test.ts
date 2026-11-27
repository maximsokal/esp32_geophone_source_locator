import { afterEach, describe, expect, it, vi } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloud';
import type { CpuDepthInformationLike, DepthFusionFrameContext } from '../../src/xr/depthPointCloudV2';
import { DepthFusionClient } from '../../src/xr/depthFusionClient';
import { createDepthFusionHandler } from '../../src/xr/depthFusionCore';
import { DepthBufferPool, captureDepthFrameBuffer, samplerFromDepthFrameBuffer } from '../../src/xr/depthFusionFrame';
import type { FusionWorkerRequest, FusionWorkerResponse } from '../../src/xr/depthFusionProtocol';
import { ARCORE_NOISE, DEFAULT_INTRINSICS, insideCornerScene, orbitPoses, renderDepth } from './support/syntheticDepthScene';

/*
 * Depth fusion worker (Phase 2, stage 1). The gate of the whole stage: what the worker fuses is BIT-IDENTICAL to what the synchronous path fuses from the
 * same frames in the same order, and the queue, the pool, the generations and the bounded waits behave as approved.
 */
const projection = Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function frames(count = 12) {
  const scene = insideCornerScene([0, 0, 0], 0.6);
  const poses = orbitPoses([0, 0, 0.15], 0.9, 35, 0, 330, count);
  return poses.map((pose, index) => ({ ...renderDepth(scene, pose, index + 1, ARCORE_NOISE, DEFAULT_INTRINSICS), pose, index }));
}

function contextOf(frame: ReturnType<typeof frames>[number], keyframe = true): DepthFusionFrameContext {
  return { keyframeId: frame.index + 1, evidenceKeyframe: keyframe, frameWeight: keyframe ? 1 : 0.2, cameraProjectPosition: frame.pose.eye };
}

function geometryOf(frame: ReturnType<typeof frames>[number]) {
  return { projectionMatrix: Float32Array.from(frame.view.projectionMatrix as ArrayLike<number>), worldFromView: Float32Array.from(frame.view.transform.matrix as ArrayLike<number>) };
}

describe('depth frame transfer', () => {
  /** A device-like depth object: raw uint16 buffer, rawValueToMeters and normDepthBufferFromNormView, exactly the fields the browser exposes. */
  function rawDepth(width: number, height: number, seed = 3): CpuDepthInformationLike & Record<string, unknown> {
    const values = new Uint16Array(width * height);
    for (let i = 0; i < values.length; i += 1) values[i] = i % 17 === 0 ? 0 : 800 + ((i * seed * 37) % 3500);
    return {
      width, height, data: values.buffer, rawValueToMeters: 0.001,
      normDepthBufferFromNormView: { matrix: Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]) },
      projectionMatrix: projection, transform: { matrix: projection },
      getDepthInMeters(x: number, y: number) { return values[Math.min(height - 1, Math.trunc(y * height)) * width + Math.min(width - 1, Math.trunc(x * width))] * 0.001; },
    };
  }

  it('the rebuilt sampler reads exactly what the device sampler reads, at every pixel and between pixels (raw buffer path)', () => {
    const depth = rawDepth(64, 36);
    const captured = captureDepthFrameBuffer(depth, 'unsigned-short')!;
    expect(captured.raw).toBe(true);
    expect(captured.data).not.toBe(depth.data);                         /* a copy: the device buffer can never be transferred */
    const sampler = samplerFromDepthFrameBuffer(captured, { projectionMatrix: projection, worldFromView: projection });
    for (let y = 0; y <= 40; y += 1) for (let x = 0; x <= 70; x += 1) {
      const u = Math.min(1, x / 70); const v = Math.min(1, y / 40);
      expect(sampler.getDepthInMeters(u, v)).toBe(depth.getDepthInMeters(u, v));
    }
  });

  it('a device without the raw buffer is sampled once at the pixel centres, which equals its nearest-pixel reads', () => {
    const scene = frames(1)[0];
    const captured = captureDepthFrameBuffer(scene.depth)!;
    expect(captured.raw).toBe(false);
    const sampler = samplerFromDepthFrameBuffer(captured, geometryOf(scene));
    for (let y = 0; y < scene.depth.height; y += 1) for (let x = 0; x < scene.depth.width; x += 1) {
      const u = (x + 0.37) / scene.depth.width; const v = (y + 0.61) / scene.depth.height;
      const expected = scene.depth.getDepthInMeters(u, v); const actual = sampler.getDepthInMeters(u, v);
      expect(Object.is(actual, expected) || (Number.isNaN(actual) && Number.isNaN(expected))).toBe(true);
    }
  });

  it('the pool is bounded: no more buffers than its capacity are ever out, and a returned buffer is reused', () => {
    const pool = new DepthBufferPool(2);
    const a = pool.acquire(100)!; const b = pool.acquire(100)!;
    expect(pool.acquire(100)).toBeNull();
    pool.release(a, 100);
    expect(pool.acquire(100)).toBe(a);
    pool.release(b, 100);
    expect(pool.inUse).toBe(1);
  });
});

describe('what the worker fuses is bit-identical to the synchronous path', () => {
  it('same frames, same order: the same stable points, normals, confidences, occupancy and counters (inline core)', async () => {
    const input = frames(16);
    const sync = new DepthPointCloudAccumulator();
    const client = new DepthFusionClient();                                      /* no worker factory: the same core inline */
    client.reset();
    for (const frame of input) {
      const keyframe = frame.index % 3 !== 2;
      sync.sampleDepthFrame(frame.depth, frame.view, contextOf(frame, keyframe));
      const outcome = client.submit({ depth: frame.depth, dataFormat: null, geometry: geometryOf(frame), context: contextOf(frame, keyframe), guidance: null, mandatory: keyframe });
      expect(outcome).toBe('accepted');
    }
    const expected = sync.snapshot(input.length, input.length);
    const actual = await client.request<typeof expected>('snapshot', { depthFrameCount: input.length, sampledDepthFrames: input.length });
    expect(actual.pointCount).toBe(expected.pointCount);
    expect(actual.pointCount).toBeGreaterThan(50);
    expect(Array.from(actual.positions)).toEqual(Array.from(expected.positions));
    expect(Array.from(actual.normals)).toEqual(Array.from(expected.normals));
    expect(Array.from(actual.confidence)).toEqual(Array.from(expected.confidence));
    expect(Array.from(actual.freeAirPositions)).toEqual(Array.from(expected.freeAirPositions));
    expect(Array.from(actual.occupiedSurfacePositions)).toEqual(Array.from(expected.occupiedSurfacePositions));
    expect(client.stats).toMatchObject({ pointCount: sync.pointCount, promotedCount: sync.promotedCount, keyframeCount: sync.keyframeCount, polishFrameCount: sync.polishFrameCount });
    expect(Array.from((await client.request<ReturnType<typeof sync.liveCoverageSnapshot>>('coverage', { budget: 4000 })).positions))
      .toEqual(Array.from(sync.liveCoverageSnapshot(4000).positions));
    client.dispose();
  });

  it('a reply is a transfer: the snapshot buffers handed to the main thread are not copied again', () => {
    const posted: Array<{ message: FusionWorkerResponse; transfer: ArrayBuffer[] }> = [];
    const { handle } = createDepthFusionHandler((message, transfer = []) => posted.push({ message, transfer }));
    handle({ type: 'reset', generation: 1 });
    for (const frame of frames(8)) {
      const buffer = captureDepthFrameBuffer(frame.depth)!;
      handle({ type: 'frame', generation: 1, seq: frame.index + 1, mandatory: true, frame: buffer, geometry: geometryOf(frame), context: contextOf(frame), guidance: null });
    }
    handle({ type: 'request', generation: 1, id: 1, kind: 'snapshot', params: { depthFrameCount: 8, sampledDepthFrames: 8 } });
    const reply = posted.at(-1)!;
    expect(reply.message.type).toBe('response');
    expect(reply.transfer.length).toBeGreaterThan(3);
    expect(posted.filter((p) => p.message.type === 'frameDone').every((p) => p.transfer.length === 1)).toBe(true);     /* the depth buffer goes back for reuse */
  });
});

class FakeWorker {
  readonly posted: Array<{ message: FusionWorkerRequest; transfer: ArrayBuffer[] }> = [];
  terminated = false;
  private readonly listeners = new Map<string, Set<(event: never) => void>>();
  addEventListener(type: string, listener: (event: never) => void): void { const set = this.listeners.get(type) ?? new Set(); set.add(listener); this.listeners.set(type, set); }
  removeEventListener(type: string, listener: (event: never) => void): void { this.listeners.get(type)?.delete(listener); }
  postMessage(message: FusionWorkerRequest, transfer: ArrayBuffer[] = []): void { this.posted.push({ message, transfer }); }
  terminate(): void { this.terminated = true; }
  emit(response: FusionWorkerResponse): void { for (const listener of this.listeners.get('message') ?? []) (listener as (e: { data: FusionWorkerResponse }) => void)({ data: response }); }
  fail(message: string): void { for (const listener of this.listeners.get('error') ?? []) (listener as (e: { message: string }) => void)({ message }); }
  frames(): Array<Extract<FusionWorkerRequest, { type: 'frame' }>> { return this.posted.map((p) => p.message).filter((m): m is Extract<FusionWorkerRequest, { type: 'frame' }> => m.type === 'frame'); }
  done(seq: number, generation: number): void {
    const frame = this.frames().find((f) => f.seq === seq)!;
    this.emit({ type: 'frameDone', generation, seq, buffer: frame.frame.data.byteLength > 0 ? frame.frame.data : null, changedPoints: 1, workMs: 1, stats: { pointCount: seq, promotedCount: 0, polishFrameCount: 0, keyframeCount: seq, integratedFrames: seq } });
  }
}

function make(options: ConstructorParameters<typeof DepthFusionClient>[0] = {}) {
  const worker = new FakeWorker();
  const client = new DepthFusionClient({ workerFactory: () => worker as unknown as Worker, maxInFlight: 2, mandatoryReserve: 2, requestTimeoutMs: 50, drainTimeoutMs: 50, ...options });
  const input = frames(40);
  let next = 0;
  const submit = (mandatory: boolean) => { const frame = input[next++ % input.length]; return client.submit({ depth: frame.depth, dataFormat: null, geometry: geometryOf(frame), context: contextOf(frame, mandatory), guidance: null, mandatory }); };
  return { worker, client, submit };
}

describe('queue policy, pool and generations', () => {
  afterEach(() => vi.useRealTimers());

  it('QUEUE: optional frames are dropped beyond the in-flight bound, keyframes use the reserve, and only a full reserve drops a keyframe (all counted)', () => {
    const { client, submit } = make();
    client.reset();
    expect([submit(false), submit(false), submit(false)]).toEqual(['accepted', 'accepted', 'dropped-optional']);
    expect([submit(true), submit(true), submit(true)]).toEqual(['accepted', 'accepted', 'dropped-keyframe']);
    expect(client.queueDepth).toBe(4);
    expect(client.counters).toMatchObject({ accepted: 4, droppedOptional: 1, droppedKeyframes: 1, maxQueueDepth: 4 });
    expect(client.overloaded).toBe(true);
  });

  it('POOL: memory is bounded by construction: the pool never holds more depth buffers than slots, and returned buffers make room again', () => {
    const { worker, client, submit } = make();
    const generation = client.reset();
    for (let i = 0; i < 30; i += 1) submit(true);
    expect(client.queueDepth).toBe(4);
    expect(worker.frames()).toHaveLength(4);
    worker.done(1, generation);
    expect(client.queueDepth).toBe(3);
    expect(submit(true)).toBe('accepted');
  });

  it('GENERATION: a late answer of an older scan is ignored (no state, no queue slot, no pending request touched)', async () => {
    const { worker, client, submit } = make();
    const old = client.reset();
    submit(true);
    const newer = client.reset();
    expect(newer).toBe(old + 1);
    expect(client.queueDepth).toBe(0);
    worker.emit({ type: 'frameDone', generation: old, seq: 1, buffer: null, changedPoints: 5, workMs: 1, stats: { pointCount: 999, promotedCount: 0, polishFrameCount: 0, keyframeCount: 1, integratedFrames: 1 } });
    expect(client.stats.pointCount).toBe(0);
    const pending = client.request('stats');
    const id = (worker.posted.at(-1)!.message as { id: number }).id;
    worker.emit({ type: 'response', generation: old, id, kind: 'stats', payload: 'stale', workMs: 0, stats: client.stats });
    client.reset();                                                                              /* supersedes the pending request */
    await expect(pending).rejects.toThrow(/superseded/);
  });

  it('FINISH: drain stops intake, waits for the accepted frames and then resolves; nothing accepted is lost', async () => {
    const { worker, client, submit } = make({ drainTimeoutMs: 5_000 });
    const generation = client.reset();
    submit(true); submit(true);
    const drained = client.drain();
    expect(submit(true)).toBe('unavailable');                                                    /* intake is closed */
    worker.done(1, generation);
    worker.done(2, generation);
    await expect(drained).resolves.toEqual({ drained: true, outstandingFrames: 0, outstandingKeyframes: 0 });
  });

  it('HUNG WORKER: drain and requests are bounded, and the outstanding keyframes are reported instead of waited on forever', async () => {
    vi.useFakeTimers();
    const { client, submit } = make({ drainTimeoutMs: 1_000, requestTimeoutMs: 1_000 });
    client.reset();
    submit(true); submit(false);
    const drained = client.drain();
    const snapshot = client.request('snapshot');
    const snapshotOutcome = snapshot.then(() => 'resolved', (error: Error) => error.message);
    await vi.advanceTimersByTimeAsync(1_001);
    await expect(drained).resolves.toEqual({ drained: false, outstandingFrames: 2, outstandingKeyframes: 1 });
    await expect(snapshotOutcome).resolves.toMatch(/timed out/);
  });

  it('CRASH: a worker error fails the client at once: pending requests reject, the slots are freed, the owner is told, new frames are refused', async () => {
    const onFailure = vi.fn();
    const { worker, client, submit } = make({ onFailure });
    client.reset();
    submit(true);
    const pending = client.request('snapshot');
    worker.fail('boom');
    await expect(pending).rejects.toThrow(/boom/);
    expect(onFailure).toHaveBeenCalledWith('boom');
    expect(client.queueDepth).toBe(0);
    expect(client.hasFailed).toBe('boom');
    expect(submit(true)).toBe('unavailable');
    expect(worker.terminated).toBe(true);
    await expect(client.request('stats')).rejects.toThrow(/failed/);
  });

  it('CANCEL: dispose terminates the worker, rejects what is pending and refuses everything afterwards', async () => {
    const { worker, client, submit } = make();
    client.reset();
    submit(true);
    const pending = client.request('stats');
    client.dispose();
    await expect(pending).rejects.toThrow(/disposed/);
    expect(worker.terminated).toBe(true);
    expect(submit(true)).toBe('unavailable');
  });

  it('CPU FALLBACK: when no worker can be started the same core runs inline and the frames are still fused', async () => {
    const throwing = new DepthFusionClient({ workerFactory: () => { throw new Error('Worker is not available'); } });
    expect(throwing.mode).toBe('inline');
    throwing.reset();
    const frame = frames(1)[0];
    expect(throwing.submit({ depth: frame.depth, dataFormat: null, geometry: geometryOf(frame), context: contextOf(frame), guidance: null, mandatory: true })).toBe('accepted');
    expect(throwing.stats.integratedFrames).toBe(1);
    expect(throwing.stats.pointCount).toBeGreaterThan(0);
  });
});
