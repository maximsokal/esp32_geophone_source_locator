import { describe, expect, it, vi } from 'vitest';

import { ContinuationBudgetExceededError, ContinuationCancelledError, ContinuationClient } from '../../src/xr/continuationClient';
import {
  CONTINUATION_TRANSIENT_BUDGET_BYTES,
  estimateContinuationBudget,
  registerAndMergeScanContinuation,
  type ScanContinuationResult,
} from '../../src/xr/scanContinuation';
import { processContinuationWorkerMessage, type ContinuationWorkerRequest, type ContinuationWorkerResponse } from '../../src/xr/continuationWorker';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from './support/validScanSnapshot';

type WorkerListener = (event: MessageEvent<ContinuationWorkerResponse> | ErrorEvent) => void;

class DeferredWorker {
  readonly listeners = new Map<string, WorkerListener>();
  readonly removeListener = new Map<string, WorkerListener>();
  readonly terminate = vi.fn(() => { this.terminated = true; });
  request: ContinuationWorkerRequest | null = null;
  transfer: Transferable[] = [];
  terminated = false;

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    const handler = typeof listener === 'function' ? listener as WorkerListener : (event) => listener.handleEvent(event as Event);
    this.listeners.set(type, handler);
    this.removeListener.set(type, handler);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (this.listeners.get(type) === listener || this.listeners.get(type) === this.removeListener.get(type)) this.listeners.delete(type);
  }

  postMessage(message: unknown, transfer: Transferable[] = []): void {
    this.request = message as ContinuationWorkerRequest;
    this.transfer = transfer;
  }

  send(response: ContinuationWorkerResponse): void {
    this.listeners.get('message')?.({ data: response } as MessageEvent<ContinuationWorkerResponse>);
  }

  fail(message: string): void {
    this.listeners.get('error')?.({ message } as ErrorEvent);
  }
}

function clientFor(worker: DeferredWorker, factory = vi.fn(() => worker)): ContinuationClient {
  return new ContinuationClient(factory as unknown as () => Worker);
}

function sizedSnapshot(points: number): ScanPointCloudSnapshot {
  const base = makeValidScanSnapshot();
  const positions = new Float32Array(points * 3);
  const normals = new Float32Array(points * 3);
  const confidence = new Float32Array(points).fill(0.92);
  const normalConfidence = new Float32Array(points).fill(0.88);
  const tsdfPositions = positions.slice();
  const tsdfNormals = normals.slice();
  return {
    ...base,
    positions,
    normals,
    confidence,
    normalConfidence,
    pointCount: points,
    rawPointCount: points,
    tentativePositions: positions.slice(),
    tentativeConfidence: confidence.slice(),
    tentativePointCount: points,
    freeAirPositions: positions.slice(),
    occupiedSurfacePositions: positions.slice(),
    unknownCandidatePositions: positions.slice(),
    knownFreeVoxelCount: points,
    occupiedSurfaceVoxelCount: points,
    unknownCandidateVoxelCount: points,
    tsdfPositions,
    tsdfNormals,
    tsdfConfidence: confidence.slice(),
    tsdfResidualMeters: confidence.slice(),
    tsdfNormalConfidence: confidence.slice(),
    tsdfCoverageConfidence: confidence.slice(),
    tsdfCoverageReady: new Uint8Array(points),
    tsdfPointCount: points,
  };
}

function pointBounds(points: Float32Array): ScanPointCloudSnapshot['bounds'] {
  if (points.length === 0) return null;
  let minX = Infinity, minY = Infinity, minZ = Infinity;
  let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let index = 0; index < points.length; index += 3) {
    minX = Math.min(minX, points[index]); minY = Math.min(minY, points[index + 1]); minZ = Math.min(minZ, points[index + 2]);
    maxX = Math.max(maxX, points[index]); maxY = Math.max(maxY, points[index + 1]); maxZ = Math.max(maxZ, points[index + 2]);
  }
  return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

function continuationSnapshot(positions: Float32Array, occupancyFineVoxelSizeMeters = 0.06): ScanPointCloudSnapshot {
  const base = makeValidScanSnapshot();
  const pointCount = positions.length / 3;
  const confidence = new Float32Array(pointCount).fill(0.92);
  const normals = new Float32Array(positions.length);
  for (let index = 2; index < normals.length; index += 3) normals[index] = 1;
  const bounds = pointBounds(positions);
  return {
    ...base,
    positions,
    confidence,
    normals,
    normalConfidence: new Float32Array(pointCount).fill(0.9),
    pointCount,
    rawPointCount: pointCount,
    bounds,
    fullBounds: bounds,
    maxPointCapacity: Math.max(pointCount, 24000),
    memoryBudgetPoints: Math.max(pointCount, 24000),
    promotedPointCount: pointCount,
    freeAirPositions: new Float32Array(0),
    occupiedSurfacePositions: new Float32Array(0),
    unknownCandidatePositions: new Float32Array(0),
    knownFreeVoxelCount: 0,
    occupiedSurfaceVoxelCount: 0,
    unknownCandidateVoxelCount: 0,
    occupancyFineVoxelSizeMeters,
    occupancyEnvelopeBounds: bounds,
    tsdfPositions: positions.slice(),
    tsdfNormals: normals.slice(),
    tsdfConfidence: confidence.slice(),
    tsdfResidualMeters: new Float32Array(pointCount).fill(0.01),
    tsdfNormalConfidence: new Float32Array(pointCount).fill(0.9),
    tsdfCoverageConfidence: new Float32Array(pointCount).fill(0.9),
    tsdfCoverageReady: new Uint8Array(pointCount).fill(1),
    tsdfPointCount: pointCount,
    manualControlPoints: [],
    operatorEdges: [],
  };
}

function randomizedContinuationPair(seed: number): { base: ScanPointCloudSnapshot; extension: ScanPointCloudSnapshot } {
  let state = seed >>> 0;
  const random = (): number => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x1_0000_0000;
  };
  const world: number[] = [];
  const overlap: number[] = [];
  for (let xIndex = 0; xIndex < 30; xIndex += 1) {
    for (let yIndex = 0; yIndex < 20; yIndex += 1) {
      const x = (2 * xIndex) / 29;
      const y = (2 * yIndex) / 19;
      const z = 0.18 * Math.sin(xIndex * 0.43 + yIndex * 0.67) +
        0.13 * Math.cos(xIndex * 0.83 - yIndex * 0.29) +
        0.04 * Math.sin(xIndex * yIndex * 0.17) + (random() - 0.5) * 0.002;
      world.push(x, y, z);
      if (xIndex >= 24) overlap.push(x, y, z);
    }
  }
  for (let index = 0; index < 300; index += 1) {
    overlap.push(5 + random() * 1.4, 3 + random() * 2, 1.2 + random() * 1.2);
  }
  const yaw = 23 * Math.PI / 180;
  const translation = [0.8, -0.5, 0.3] as const;
  const local = new Float32Array(overlap.length);
  const cosine = Math.cos(yaw), sine = Math.sin(yaw);
  for (let offset = 0; offset < local.length; offset += 3) {
    const x = overlap[offset] - translation[0];
    const y = overlap[offset + 1] - translation[1];
    local[offset] = cosine * x + sine * y;
    local[offset + 1] = -sine * x + cosine * y;
    local[offset + 2] = overlap[offset + 2] - translation[2];
  }
  return { base: continuationSnapshot(new Float32Array(world)), extension: continuationSnapshot(local) };
}

function establishedPartialOverlapFixture(): { base: ScanPointCloudSnapshot; extension: ScanPointCloudSnapshot } {
  const world: number[] = [];
  const overlap: number[] = [];
  for (let xIndex = 0; xIndex < 30; xIndex += 1) {
    for (let yIndex = 0; yIndex < 20; yIndex += 1) {
      const x = (2 * xIndex) / 29;
      const y = (2 * yIndex) / 19;
      const z = 0.18 * Math.sin(xIndex * 0.43 + yIndex * 0.67) +
        0.13 * Math.cos(xIndex * 0.83 - yIndex * 0.29) +
        0.04 * Math.sin(xIndex * yIndex * 0.17);
      world.push(x, y, z);
      if (xIndex >= 24) overlap.push(x, y, z);
    }
  }
  const yaw = 23 * Math.PI / 180;
  const translation = [0.8, -0.5, 0.3] as const;
  const cosine = Math.cos(yaw), sine = Math.sin(yaw);
  const local = new Float32Array(overlap.length);
  for (let offset = 0; offset < local.length; offset += 3) {
    const x = overlap[offset] - translation[0];
    const y = overlap[offset + 1] - translation[1];
    local[offset] = cosine * x + sine * y;
    local[offset + 1] = -sine * x + cosine * y;
    local[offset + 2] = overlap[offset + 2] - translation[2];
  }
  return {
    base: { ...continuationSnapshot(new Float32Array(world), 0.18), voxelSizeMeters: 0.04 },
    extension: { ...continuationSnapshot(local, 0.18), voxelSizeMeters: 0.04 },
  };
}

class InProcessContinuationWorker extends DeferredWorker {
  postMessage(message: unknown, transfer: Transferable[] = []): void {
    super.postMessage(message, transfer);
    const request = structuredClone(message, { transfer }) as ContinuationWorkerRequest;
    queueMicrotask(() => {
      if (this.terminated) return;
      processContinuationWorkerMessage(request, {
        postMessage: (response, responseTransfer = []) => {
          const ownedByMain = structuredClone(response, { transfer: responseTransfer }) as ContinuationWorkerResponse;
          this.send(ownedByMain);
        },
      });
    });
  }
}

describe('ContinuationClient', () => {
  it('returns a pending promise while worker compute is outstanding and transfers only request copies', async () => {
    const worker = new DeferredWorker();
    const client = clientFor(worker);
    const base = makeValidScanSnapshot();
    const extension = makeValidScanSnapshot();
    const baseBuffer = base.positions.buffer;
    let settled = false;
    const pending = client.continue(base, extension).finally(() => { settled = true; });
    await Promise.resolve();

    expect(settled).toBe(false);
    expect(worker.request?.type).toBe('continue');
    expect(worker.request?.base.positions).not.toBe(base.positions);
    expect(worker.transfer).toContain(worker.request?.base.positions.buffer);
    expect(base.positions.buffer).toBe(baseBuffer);
    expect(base.positions.byteLength).toBeGreaterThan(0);

    client.cancel('test cancellation');
    await expect(pending).rejects.toBeInstanceOf(ContinuationCancelledError);
    expect(worker.terminate).toHaveBeenCalledTimes(1);
  });

  it('carries the tag-derived seed to the worker, and nothing when there is none', async () => {
    const worker = new DeferredWorker();
    const client = clientFor(worker);
    const base = makeValidScanSnapshot();
    const extension = makeValidScanSnapshot();
    const seed = { yaw: 0.7, tx: 2.5, ty: -1.2, tz: 0.04 };
    void client.continue(base, extension, { seed }).catch(() => undefined);
    await Promise.resolve();
    expect(worker.request?.options.seed).toEqual(seed);
    client.cancel('test');

    const second = new DeferredWorker();
    const other = clientFor(second);
    void other.continue(makeValidScanSnapshot(), makeValidScanSnapshot()).catch(() => undefined);
    await Promise.resolve();
    expect(second.request?.options).not.toHaveProperty('seed');
    other.cancel('test');
  });

  it('estimates each array family and rejects an over-budget request before creating or posting to a worker', async () => {
    const base = sizedSnapshot(117_305);
    const extension = sizedSnapshot(117_305);
    const estimate = estimateContinuationBudget(base, extension);
    const familyTotal = (...families: Array<keyof typeof estimate.families>): number =>
      families.reduce((sum, family) => sum + estimate.families[family].totalBytes, 0);
    expect({
      stable: familyTotal('stablePositions', 'stableConfidence', 'stableNormals', 'stableNormalConfidence'),
      tentative: familyTotal('tentativePositions', 'tentativeConfidence'),
      free: familyTotal('freeAirPositions'),
      occupied: familyTotal('occupiedSurfacePositions'),
      unknown: familyTotal('unknownCandidatePositions'),
      tsdf: familyTotal('tsdfPositions', 'tsdfNormals', 'tsdfConfidence', 'tsdfResidualMeters', 'tsdfNormalConfidence', 'tsdfCoverageConfidence', 'tsdfCoverageReady'),
      annotationJson: estimate.families.annotationJson.totalBytes,
      registrationScratch: estimate.families.registrationScratch.totalBytes,
    }).toEqual({
      stable: 60_998_600,
      tentative: 31_437_740,
      free: 44_575_900,
      occupied: 44_575_900,
      unknown: 23_455_320,
      tsdf: 59_590_940,
      annotationJson: 680,
      registrationScratch: 3_801_216,
    });
    expect(estimate.estimatedBytes).toBe(268_436_296);
    expect(estimate.limitBytes).toBe(CONTINUATION_TRANSIENT_BUDGET_BYTES);
    expect(estimate.estimatedBytes).toBeGreaterThan(estimate.limitBytes);
    expect(estimate.families.stablePositions.inputCopyBytes).toBe(117_305 * 3 * 4 * 2);
    expect(estimate.families.occupiedSurfacePositions.inputCopyBytes).toBe(117_305 * 3 * 4 * 2);
    expect(estimate.families.tsdfPositions.inputCopyBytes).toBe(117_305 * 3 * 4 * 2);
    expect(estimate.families.tsdfCoverageReady.inputCopyBytes).toBe(117_305 * 2);
    expect(estimate.families.annotationJson.inputCopyBytes).toBeGreaterThan(0);
    const factory = vi.fn(() => new DeferredWorker() as unknown as Worker);
    const client = new ContinuationClient(factory);

    await expect(client.continue(base, extension)).rejects.toMatchObject({
      code: 'continuation_transient_budget_exceeded',
      estimate: { estimatedBytes: estimate.estimatedBytes },
    } satisfies Partial<ContinuationBudgetExceededError>);
    expect(factory).not.toHaveBeenCalled();

    const smallBase = makeValidScanSnapshot();
    const smallExtension = makeValidScanSnapshot();
    const smallEstimate = estimateContinuationBudget(smallBase, smallExtension);
    expect(smallEstimate.estimatedBytes).toBeLessThan(smallEstimate.limitBytes);
  });

  it('ignores a late generation, detaches its output buffers, and terminates the superseded worker', async () => {
    const workers: DeferredWorker[] = [];
    const client = new ContinuationClient(() => {
      const worker = new DeferredWorker();
      workers.push(worker);
      return worker as unknown as Worker;
    });
    const first = client.continue(makeValidScanSnapshot(), makeValidScanSnapshot(), { generation: 10 });
    const workerA = workers[0];
    const lateHandler = workerA.listeners.get('message');
    const requestA = workerA.request!;
    const requestCopyBuffer = requestA.base.positions.buffer as ArrayBuffer;
    const firstSettled = expect(first).rejects.toBeInstanceOf(ContinuationCancelledError);

    const second = client.continue(makeValidScanSnapshot(), makeValidScanSnapshot(), { generation: 11 });
    const stale = makeValidScanSnapshot();
    const staleResult: ScanContinuationResult = {
      ok: true,
      snapshot: stale,
      diagnostics: { yawRadians: 0, translation: [0, 0, 0], inliers: 120, overlapRatio: 0.4, rmsMeters: 0, baseSampleCount: 120, extensionSampleCount: 120 },
      extensionToBase: { yaw: 0, tx: 0, ty: 0, tz: 0 },
    };
    lateHandler?.({ data: { type: 'result', requestId: requestA.requestId, generation: requestA.generation, result: staleResult } } as MessageEvent<ContinuationWorkerResponse>);

    expect(workerA.terminated).toBe(true);
    expect(requestCopyBuffer.byteLength).toBe(0);
    expect(stale.positions.byteLength).toBe(0);
    await firstSettled;
    const workerB = workers[1];
    workerB.send({
      type: 'result', requestId: workerB.request!.requestId, generation: 11,
      result: {
        ok: false, reason: 'merged estimate rejected',
        diagnostics: {
          yawRadians: 0, translation: [0, 0, 0], inliers: 120, overlapRatio: 0.4,
          rmsMeters: 0, baseSampleCount: 120, extensionSampleCount: 120,
          rejectionReason: 'merged_budget_exceeded',
        },
      },
    });
    await expect(second).resolves.toMatchObject({
      ok: false, reason: 'merged estimate rejected', diagnostics: { rejectionReason: 'merged_budget_exceeded' },
    });
  });

  it('matches direct decisions and canonical geometry through the worker entry for a seeded randomized overlap', async () => {
    const fixture = randomizedContinuationPair(0x41c0ffee);
    const direct = registerAndMergeScanContinuation(fixture.base, fixture.extension);
    const worker = new InProcessContinuationWorker();
    const client = clientFor(worker);
    const phases: string[] = [];
    const throughClient = await client.continue(fixture.base, fixture.extension, { generation: 42, onPhase: (phase) => phases.push(phase) });

    expect(throughClient.ok).toBe(direct.ok);
    expect(phases).toEqual(['registration', 'stable', 'occupancy', 'tsdf']);
    if (!direct.ok || !throughClient.ok) {
      expect(throughClient).toMatchObject({ ok: false, reason: direct.ok ? undefined : direct.reason });
      return;
    }
    expect(throughClient.diagnostics.inliers).toBe(direct.diagnostics.inliers);
    expect(throughClient.diagnostics.rmsMeters).toBeCloseTo(direct.diagnostics.rmsMeters, 8);
    expect(throughClient.extensionToBase).toEqual(direct.extensionToBase);
    const canonicalVoxelKeys = (positions: Float32Array, voxel: number): string[] => {
      const keys: string[] = [];
      for (let offset = 0; offset + 2 < positions.length; offset += 3) {
        keys.push(`${Math.floor(positions[offset] / voxel)}:${Math.floor(positions[offset + 1] / voxel)}:${Math.floor(positions[offset + 2] / voxel)}`);
      }
      return keys.sort();
    };
    for (const key of [
      'positions', 'tentativePositions', 'freeAirPositions', 'occupiedSurfacePositions',
      'unknownCandidatePositions', 'tsdfPositions',
    ] as const) {
      expect(canonicalVoxelKeys(throughClient.snapshot[key], fixture.base.voxelSizeMeters)).toEqual(
        canonicalVoxelKeys(direct.snapshot[key], fixture.base.voxelSizeMeters),
      );
    }
  });

  it('matches the established partial-overlap fixture and insufficient-data rejection through the worker entry', async () => {
    const accepted = establishedPartialOverlapFixture();
    const directAccepted = registerAndMergeScanContinuation(accepted.base, accepted.extension);
    const workerAccepted = await clientFor(new InProcessContinuationWorker()).continue(accepted.base, accepted.extension);
    expect(directAccepted.ok).toBe(true);
    expect(workerAccepted.ok).toBe(directAccepted.ok);
    if (directAccepted.ok && workerAccepted.ok) {
      expect(workerAccepted.diagnostics.inliers).toBe(directAccepted.diagnostics.inliers);
      expect(workerAccepted.diagnostics.rmsMeters).toBeCloseTo(directAccepted.diagnostics.rmsMeters, 8);
      expect(workerAccepted.extensionToBase).toEqual(directAccepted.extensionToBase);
    }

    const insufficient = makeValidScanSnapshot();
    const directRejected = registerAndMergeScanContinuation(insufficient, insufficient);
    const workerRejected = await clientFor(new InProcessContinuationWorker()).continue(insufficient, insufficient);
    expect(directRejected.ok).toBe(false);
    expect(workerRejected).toEqual(directRejected);
  });
});
