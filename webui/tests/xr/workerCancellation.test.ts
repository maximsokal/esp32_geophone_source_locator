import { afterEach, describe, expect, it, vi } from 'vitest';

import { LiveComputeScheduler } from '../../src/xr/liveComputeScheduler';
import { assessMeshReconstructionPreflight } from '../../src/xr/meshReconstructionPreflight';
import { buildRgbDepthGuidanceFrameFromLuma } from '../../src/xr/rgbDepthGuidance';
import { SparseTsdfMeshClient, type SparseTsdfReconstructInput } from '../../src/xr/sparseTsdfClient';
import { StructuralGuideClient, type StructuralGuideWorkerResult } from '../../src/xr/structuralGuideClient';
import type { SparseTsdfMesh } from '../../src/xr/sparseTsdf';
import { TagDetectorClient, type TagCameraFrame } from '../../src/xr/fiducial/tagDetectorClient';
import { VisualLineEvidenceClient } from '../../src/xr/visualLineEvidenceClient';

type WorkerMessage = Record<string, unknown>;
type WorkerKind = 'structural' | 'visual' | 'tsdf' | 'tag';

class HoldingWorker {
  terminated = false;
  cpuActive = false;
  activeAtTermination: number | null = null;
  cpuActiveAtTermination: boolean | null = null;
  queued: WorkerMessage[] = [];
  readonly messages: WorkerMessage[] = [];
  private epoch = 0;
  private current: WorkerMessage | null = null;
  private lastResponse: WorkerMessage | null = null;
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();
  private readonly errorListeners = new Set<(event: ErrorEvent) => void>();
  private resolveStarted!: () => void;
  readonly started = new Promise<void>((resolve) => { this.resolveStarted = resolve; });

  constructor(
    private readonly kind: WorkerKind,
    private readonly activeSlots: () => number = () => 0,
  ) {}

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.add(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.add(listener as (event: ErrorEvent) => void);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.delete(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.delete(listener as (event: ErrorEvent) => void);
  }

  postMessage(value: unknown): void {
    const message = value as WorkerMessage;
    this.messages.push(message);
    if (this.cpuActive) {
      this.queued.push(message);
      return;
    }
    if (message.type === 'reset') {
      this.epoch = Number(message.epoch);
      return;
    }
    if (message.type !== 'analyze' && message.type !== 'reconstruct' && !(this.kind === 'tag' && message.type === 'detect')) return;
    this.current = message;
    this.cpuActive = true;
    this.resolveStarted();
  }

  terminate(): void {
    this.activeAtTermination = this.activeSlots();
    this.cpuActiveAtTermination = this.cpuActive;
    this.terminated = true;
    this.cpuActive = false;
    this.queued = [];
  }

  completeCurrent(): void {
    const request = this.current;
    if (request === null) return;
    this.cpuActive = false;
    this.lastResponse = this.responseFor(request);
    this.emit(this.lastResponse);
    this.current = null;
  }

  /** Simulates a response already queued by the worker before terminate(). */
  replayLateResponse(): void {
    const response = this.lastResponse ?? (this.current === null ? null : this.responseFor(this.current));
    if (response !== null) this.emit(response);
  }

  private responseFor(request: WorkerMessage): WorkerMessage {
    const requestId = Number(request.requestId);
    const epoch = this.epoch;
    if (this.kind === 'structural') {
      return {
        type: 'result',
        requestId,
        epoch,
        result: {
          edges: [],
          corners: [],
          computeMs: 1,
          testScanId: Number(request.keyframeCount),
        } as unknown as StructuralGuideWorkerResult,
      };
    }
    if (this.kind === 'visual') {
      return { type: 'result', requestId, epoch, lines: [], junctions: [] };
    }
    if (this.kind === 'tag') return { type: 'result', requestId, epoch, detections: [] };
    return {
      type: 'result',
      requestId,
      epoch,
      result: { trianglePositions: new Float32Array(0), triangleCount: 0 } as SparseTsdfMesh,
    };
  }

  private emit(data: WorkerMessage): void {
    const event = { data } as MessageEvent;
    for (const listener of this.messageListeners) listener(event);
  }
}

class ImmediatePreflightWorker {
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (type === 'message' && typeof listener === 'function') {
      this.messageListeners.add(listener as (event: MessageEvent) => void);
    }
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (type === 'message' && typeof listener === 'function') {
      this.messageListeners.delete(listener as (event: MessageEvent) => void);
    }
  }

  postMessage(value: unknown): void {
    const message = value as WorkerMessage;
    if (message.type !== 'preflight') return;
    const positions = message.positions as Float32Array;
    const normals = message.normals as Float32Array;
    const confidence = message.confidence as Float32Array;
    const result = assessMeshReconstructionPreflight({
      positions,
      normals,
      confidence,
      voxelSizeMeters: Number(message.voxelSizeMeters),
    });
    const response = {
      type: 'preflight-result',
      requestId: Number(message.requestId),
      positions,
      normals,
      confidence,
      result,
      computeMs: 1,
    };
    queueMicrotask(() => {
      const event = { data: response } as MessageEvent;
      for (const listener of this.messageListeners) listener(event);
    });
  }

  terminate(): void {}
}

const identity = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);

function structuralInput(scanId = 1) {
  return {
    positions: new Float32Array([0, 0, 0]),
    normals: new Float32Array([0, 0, 1]),
    confidence: new Float32Array([1]),
    keyframeCount: scanId,
    voxelSizeMeters: 0.04,
  } as Parameters<StructuralGuideClient['analyze']>[0];
}

function visualFrame() {
  const width = 64;
  const height = 48;
  const luma = new Uint8Array(width * height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) luma[y * width + x] = x < 31 ? 20 : 235;
  }
  return buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 100);
}

function tagFrame(): TagCameraFrame {
  return {
    width: 2,
    height: 2,
    rgba: new Uint8Array(16),
    capturedAtMs: 100,
    cameraPosition: [0, 0, 0],
    cameraForward: [0, 0, -1],
    projectionMatrix: identity,
    worldFromView: identity,
  };
}

function gridInput(width: number, height: number): SparseTsdfReconstructInput {
  const positions: number[] = [];
  const normals: number[] = [];
  const confidence: number[] = [];
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      positions.push(x * 0.04, y * 0.04, 0);
      normals.push(0, 0, 1);
      confidence.push(0.92);
    }
  }
  return {
    positions: Float32Array.from(positions),
    normals: Float32Array.from(normals),
    confidence: Float32Array.from(confidence),
    voxelSizeMeters: 0.04,
  };
}

async function waitFor(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (predicate()) return;
    await Promise.resolve();
  }
  throw new Error('Worker did not reach the expected state.');
}

function visualClient(workerFactory: () => Worker, scheduler: LiveComputeScheduler): VisualLineEvidenceClient {
  return new VisualLineEvidenceClient(
    { allowMainThreadFallback: false, deferWorkerUntilAnalyze: true },
    workerFactory,
    scheduler,
  );
}

describe('synchronous XR worker cancellation', () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it('terminates tag detector CPU work before reset rejects it and recreates lazily', async () => {
    const workers: HoldingWorker[] = [];
    const client = new TagDetectorClient(() => {
      const worker = new HoldingWorker('tag');
      workers.push(worker);
      return worker as unknown as Worker;
    });

    const oldJob = client.detect(tagFrame());
    const oldJobResult = oldJob.then(
      () => ({ rejected: false, message: '' }),
      (error: unknown) => ({ rejected: true, message: error instanceof Error ? error.message : String(error) }),
    );
    await waitFor(() => workers[0]?.cpuActive === true);
    client.reset();

    expect(workers[0].terminated).toBe(true);
    expect(workers[0].cpuActiveAtTermination).toBe(true);
    const oldOutcome = await oldJobResult;
    expect(oldOutcome.rejected).toBe(true);
    expect(oldOutcome.message).toContain('reset for a new XR session');
    expect(workers).toHaveLength(1);
    expect(client.workerBacked).toBe(false);

    const nextJob = client.detect(tagFrame());
    await waitFor(() => workers.length === 2 && workers[1].cpuActive);
    expect(workers[1].messages.slice(0, 2).map((message) => message.type)).toEqual(['reset', 'detect']);
    expect(workers[1].messages[0].epoch).toBe(2);
    expect(workers[1].messages[1].epoch).toBe(2);
    workers[1].completeCurrent();
    await expect(nextJob).resolves.toMatchObject({ detections: [] });

    client.dispose();
    expect(workers[1].terminated).toBe(true);
  });

  it('terminates structural CPU work before reset releases its scheduler slot and recreates lazily', async () => {
    const scheduler = new LiveComputeScheduler(async () => 1);
    const workers = [new HoldingWorker('structural', () => scheduler.snapshot().active), new HoldingWorker('structural', () => scheduler.snapshot().active)];
    let workerIndex = 0;
    const client = new StructuralGuideClient(() => workers[workerIndex++] as unknown as Worker, scheduler);

    const oldJob = client.analyze(structuralInput(1));
    const oldJobResult = oldJob.then(
      () => ({ rejected: false, message: '' }),
      (error: unknown) => ({ rejected: true, message: error instanceof Error ? error.message : String(error) }),
    );
    await waitFor(() => workers[0].cpuActive);
    expect(scheduler.snapshot().active).toBe(1);

    client.reset();

    expect(workers[0].terminated).toBe(true);
    expect(workers[0].activeAtTermination).toBe(1);
    const oldOutcome = await oldJobResult;
    expect(oldOutcome.rejected).toBe(true);
    expect(oldOutcome.message).toContain('reset for a new scan');
    expect(scheduler.snapshot().active).toBe(0);

    const nextJob = client.analyze(structuralInput(2));
    await waitFor(() => workers[1].cpuActive);
    expect(workers[1]).not.toBe(workers[0]);
    expect(workers[1].messages.slice(0, 2).map((message) => message.type)).toEqual(['reset', 'analyze']);
    expect(workers[1].messages[0].epoch).toBe(1);
    client.dispose();
    expect(workers[1].terminated).toBe(true);
    expect(workers[1].activeAtTermination).toBe(1);
    await expect(nextJob).rejects.toThrow('disposed');
    expect(scheduler.snapshot().active).toBe(0);
  });

  it('terminates visual CPU work before reset releases its scheduler slot and recreates lazily', async () => {
    vi.stubGlobal('Worker', class {});
    const scheduler = new LiveComputeScheduler(async () => 2);
    const workers = [new HoldingWorker('visual', () => scheduler.snapshot().active), new HoldingWorker('visual', () => scheduler.snapshot().active)];
    let workerIndex = 0;
    const client = visualClient(() => workers[workerIndex++] as unknown as Worker, scheduler);

    const oldJob = client.analyze(visualFrame(), 1, identity, identity);
    const oldJobResult = oldJob.then(
      () => ({ rejected: false, message: '' }),
      (error: unknown) => ({ rejected: true, message: error instanceof Error ? error.message : String(error) }),
    );
    await waitFor(() => workers[0].cpuActive);
    expect(scheduler.snapshot().active).toBe(1);

    client.reset();

    expect(workers[0].terminated).toBe(true);
    expect(workers[0].activeAtTermination).toBe(1);
    const oldOutcome = await oldJobResult;
    expect(oldOutcome.rejected).toBe(true);
    expect(oldOutcome.message).toContain('reset for a new scan');
    expect(scheduler.snapshot().active).toBe(0);

    const nextJob = client.analyze(visualFrame(), 2, identity, identity);
    await waitFor(() => workers[1].cpuActive);
    expect(workers[1]).not.toBe(workers[0]);
    client.dispose();
    expect(workers[1].terminated).toBe(true);
    expect(workers[1].activeAtTermination).toBe(1);
    await expect(nextJob).rejects.toThrow('disposed');
    expect(scheduler.snapshot().active).toBe(0);
  });

  it('terminates a superseded sequential TSDF worker before the short reconstruction starts', async () => {
    const workers: HoldingWorker[] = [];
    const client = new SparseTsdfMeshClient(
      () => {
        const worker = new HoldingWorker('tsdf');
        workers.push(worker);
        return worker as unknown as Worker;
      },
      () => new ImmediatePreflightWorker() as unknown as Worker,
    );

    const oldJob = client.reconstruct(gridInput(48, 48), { postScanWorkerBudget: 1 });
    await waitFor(() => workers.length === 1 && workers[0].cpuActive);
    const shortJob = client.reconstruct(gridInput(9, 9), { postScanWorkerBudget: 1 });

    await expect(oldJob).rejects.toThrow('superseded by a newer request');
    await waitFor(() => workers.length === 2 && workers[1].cpuActive);
    expect(workers[0].terminated).toBe(true);
    expect(workers[0].cpuActive).toBe(false);
    expect(workers[1].terminated).toBe(false);

    client.dispose();
    expect(workers[1].terminated).toBe(true);
    await expect(shortJob).rejects.toThrow('disposed');
  });

  it('bounds live heavy workers and rejects stale results through 100 seeded scan resets', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('Worker', class {});
    const scheduler = new LiveComputeScheduler(async () => 2);
    const structuralWorkers: HoldingWorker[] = [];
    const visualWorkers: HoldingWorker[] = [];
    const structural = new StructuralGuideClient(() => {
      const worker = new HoldingWorker('structural', () => scheduler.snapshot().active);
      structuralWorkers.push(worker);
      return worker as unknown as Worker;
    }, scheduler);
    const visual = visualClient(() => {
      const worker = new HoldingWorker('visual', () => scheduler.snapshot().active);
      visualWorkers.push(worker);
      return worker as unknown as Worker;
    }, scheduler);
    let seed = 0x396;
    const random = (maximum: number): number => {
      seed ^= seed << 13;
      seed ^= seed >>> 17;
      seed ^= seed << 5;
      return (seed >>> 0) % maximum;
    };
    const liveCounts: number[] = [];

    for (let cycle = 1; cycle <= 100; cycle += 1) {
      const structuralJob = structural.analyze(structuralInput(cycle)).then(
        (result) => ({ resolved: true, result }),
        () => ({ resolved: false, result: null }),
      );
      const visualJob = visual.analyze(visualFrame(), cycle, identity, identity).then(
        (result) => ({ resolved: true, keyframeId: result.keyframeId }),
        () => ({ resolved: false, keyframeId: null }),
      );
      await waitFor(() => structuralWorkers.at(-1)?.cpuActive === true && visualWorkers.at(-1)?.cpuActive === true);
      expect(scheduler.snapshot().active).toBeLessThanOrEqual(2);
      expect([...structuralWorkers, ...visualWorkers].filter((worker) => worker.cpuActive && !worker.terminated).length).toBeLessThanOrEqual(2);

      const oldStructuralWorker = structuralWorkers.at(-1)!;
      const oldVisualWorker = visualWorkers.at(-1)!;
      const resetDelay = random(5);
      const completionDelay = random(5);
      const resetFirst = random(2) === 0;
      const resetWins = resetDelay < completionDelay || (resetDelay === completionDelay && resetFirst);
      const reset = (): void => { structural.reset(); visual.reset(); };
      const completeOld = (): void => {
        oldStructuralWorker.completeCurrent();
        oldVisualWorker.completeCurrent();
      };
      if (resetFirst) {
        setTimeout(reset, resetDelay);
        setTimeout(completeOld, completionDelay);
      } else {
        setTimeout(completeOld, completionDelay);
        setTimeout(reset, resetDelay);
      }
      await vi.advanceTimersByTimeAsync(Math.max(resetDelay, completionDelay));

      const oldResults = await Promise.all([structuralJob, visualJob]);
      expect(oldStructuralWorker.terminated).toBe(true);
      expect(oldVisualWorker.terminated).toBe(true);
      expect(oldStructuralWorker.cpuActive).toBe(false);
      expect(oldVisualWorker.cpuActive).toBe(false);
      expect(scheduler.snapshot().active).toBe(0);
      expect([...structuralWorkers, ...visualWorkers].filter((worker) => !worker.terminated)).toHaveLength(0);
      expect(oldResults.every((outcome) => outcome.resolved === !resetWins)).toBe(true);

      const nextStructuralJob = structural.analyze(structuralInput(cycle + 1000)).then((result) => {
        return { resolved: true, scanId: Number((result as StructuralGuideWorkerResult & { testScanId: number }).testScanId) };
      }, () => ({ resolved: false, scanId: -1 }));
      const nextVisualJob = visual.analyze(visualFrame(), cycle + 1000, identity, identity).then(
        (result) => ({ resolved: true, keyframeId: result.keyframeId }),
        () => ({ resolved: false, keyframeId: -1 }),
      );
      await waitFor(() => structuralWorkers.at(-1)?.cpuActive === true && visualWorkers.at(-1)?.cpuActive === true);
      const nextStructuralWorker = structuralWorkers.at(-1)!;
      const nextVisualWorker = visualWorkers.at(-1)!;
      expect(nextStructuralWorker).not.toBe(oldStructuralWorker);
      expect(nextVisualWorker).not.toBe(oldVisualWorker);
      expect(scheduler.snapshot().active).toBeLessThanOrEqual(2);
      oldStructuralWorker.replayLateResponse();
      oldVisualWorker.replayLateResponse();
      await Promise.resolve();
      expect(nextStructuralWorker.cpuActive).toBe(true);
      expect(nextVisualWorker.cpuActive).toBe(true);

      const completionDelayB = random(4) + 1;
      setTimeout(() => {
        nextStructuralWorker.completeCurrent();
        nextVisualWorker.completeCurrent();
      }, completionDelayB);
      await vi.advanceTimersByTimeAsync(completionDelayB);
      const nextResults = await Promise.all([nextStructuralJob, nextVisualJob]);
      expect(nextResults).toEqual([
        { resolved: true, scanId: cycle + 1000 },
        { resolved: true, keyframeId: cycle + 1000 },
      ]);
      expect(scheduler.snapshot().active).toBe(0);
      const liveWorkers = [...structuralWorkers, ...visualWorkers].filter((worker) => !worker.terminated);
      expect(liveWorkers).toHaveLength(2);
      liveCounts.push(liveWorkers.length);
    }

    expect(liveCounts).toHaveLength(100);
    expect(new Set(liveCounts)).toEqual(new Set([2]));
    structural.dispose();
    visual.dispose();
    expect([...structuralWorkers, ...visualWorkers].filter((worker) => !worker.terminated)).toHaveLength(0);
    expect(scheduler.snapshot().active).toBe(0);
  });
});
