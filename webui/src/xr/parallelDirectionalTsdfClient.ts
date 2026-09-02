import { clientDiagnostics } from '../data/clientDiagnostics';
import {
  finalizeDirectionalTsdfPlan,
  type DirectionalTsdfMesh,
  type DirectionalTsdfPlanSummary,
} from './directionalTsdf';
import DirectionalTsdfMergeWorker from './directionalTsdfMerge.worker?worker&inline';
import DirectionalTsdfPlanWorker from './directionalTsdfPlan.worker?worker&inline';
import SparseTsdfChannelWorker from './sparseTsdfChannel.worker?worker&inline';
import type { SparseTsdfInput, SparseTsdfMesh } from './sparseTsdf';

interface PlanResultMessage {
  type: 'plan-result';
  requestId: number;
  summary: DirectionalTsdfPlanSummary;
  channelInputs: SparseTsdfInput[];
}

interface ChannelResultMessage {
  type: 'channel-result';
  requestId: number;
  channelIndex: number;
  result: SparseTsdfMesh;
}

interface MergeResultMessage {
  type: 'merge-result';
  requestId: number;
  result: DirectionalTsdfMesh;
}

const MAX_CONCURRENT_TSDF_CHANNEL_WORKERS = 3;

function finiteInteger(value: number, fallback = 1): number {
  if (!Number.isFinite(value)) return fallback;
  return Math.max(0, Math.floor(value));
}

/**
 * TSDF workers are memory-heavy (sparse node Maps + topology indexes), so CPU
 * availability is not the only constraint. Use at most three simultaneously
 * even when hardwareConcurrency/postScanWorkerBudget reports more capacity.
 */
export function recommendedParallelTsdfChannelWorkers(
  postScanWorkerBudget: number,
  directionalChannelCount: number,
): number {
  const workers = finiteInteger(postScanWorkerBudget, 0);
  const channels = finiteInteger(directionalChannelCount, 0);
  if (workers < 2 || channels < 2) return 1;
  return Math.min(MAX_CONCURRENT_TSDF_CHANNEL_WORKERS, workers, channels);
}

function addTransferBuffer(
  transfer: Transferable[],
  seen: Set<ArrayBuffer>,
  buffer: ArrayBufferLike,
): void {
  if (!(buffer instanceof ArrayBuffer) || seen.has(buffer)) return;
  seen.add(buffer);
  transfer.push(buffer);
}

function inputTransfers(input: SparseTsdfInput): Transferable[] {
  const transfer: Transferable[] = [];
  const seen = new Set<ArrayBuffer>();
  addTransferBuffer(transfer, seen, input.positions.buffer);
  addTransferBuffer(transfer, seen, input.normals.buffer);
  addTransferBuffer(transfer, seen, input.confidence.buffer);
  if (input.freeSpacePositions !== undefined) addTransferBuffer(transfer, seen, input.freeSpacePositions.buffer);
  return transfer;
}

export class ParallelDirectionalTsdfClient {
  private nextRequestId = 1;
  private generation = 0;
  private readonly activeWorkers = new Set<Worker>();
  private readonly pendingRejectors = new Set<(reason: unknown) => void>();

  async reconstruct(
    input: SparseTsdfInput,
    postScanWorkerBudget: number,
  ): Promise<DirectionalTsdfMesh> {
    this.reset('Parallel directional TSDF reconstruction superseded by a newer scan.');
    const generation = this.generation;
    const started = performance.now();
    const planned = await this.plan(input);
    this.assertGeneration(generation);

    const concurrency = recommendedParallelTsdfChannelWorkers(
      postScanWorkerBudget,
      planned.summary.directionalChannelCount,
    );
    clientDiagnostics.info('SCAN_TSDF_PARALLEL_PLAN', {
      mode: planned.summary.mode,
      channels: planned.summary.directionalChannelCount,
      channelJobs: planned.channelInputs.length,
      concurrentWorkers: concurrency,
      postScanWorkerBudget: finiteInteger(postScanWorkerBudget, 0),
    });

    const meshes = await this.runChannels(planned.channelInputs, concurrency);
    this.assertGeneration(generation);

    let result: DirectionalTsdfMesh;
    if (planned.summary.mode === 'multi') {
      result = await this.merge(planned.summary, meshes);
    } else {
      // Scalar/single finalization only adjusts metadata and performs no topology
      // work, so avoid paying for an otherwise empty merge Worker startup.
      result = finalizeDirectionalTsdfPlan(planned.summary, meshes);
    }
    this.assertGeneration(generation);
    result.computeMs = performance.now() - started;
    return result;
  }

  reset(reason = 'Parallel directional TSDF reconstruction reset.'): void {
    this.generation += 1;
    const error = new Error(reason);
    for (const reject of [...this.pendingRejectors]) reject(error);
    for (const worker of [...this.activeWorkers]) worker.terminate();
    this.pendingRejectors.clear();
    this.activeWorkers.clear();
  }

  dispose(): void {
    this.reset('Parallel directional TSDF worker pool disposed.');
  }

  private assertGeneration(generation: number): void {
    if (generation !== this.generation) {
      throw new Error('Parallel directional TSDF reconstruction superseded.');
    }
  }

  private async plan(input: SparseTsdfInput): Promise<{
    summary: DirectionalTsdfPlanSummary;
    channelInputs: SparseTsdfInput[];
  }> {
    const requestId = this.nextRequestId++;
    const worker = new DirectionalTsdfPlanWorker();
    return this.runOneShotWorker<PlanResultMessage, { summary: DirectionalTsdfPlanSummary; channelInputs: SparseTsdfInput[] }>(
      worker,
      (message) => message?.type === 'plan-result' && message.requestId === requestId,
      (message) => ({ summary: message.summary, channelInputs: message.channelInputs }),
      () => worker.postMessage({ type: 'plan', requestId, input }, inputTransfers(input)),
    );
  }

  private async runChannels(
    channelInputs: readonly SparseTsdfInput[],
    concurrency: number,
  ): Promise<SparseTsdfMesh[]> {
    const results = new Array<SparseTsdfMesh>(channelInputs.length);
    let nextChannel = 0;
    const workerCount = Math.max(1, Math.min(channelInputs.length, concurrency));
    const runners = Array.from({ length: workerCount }, async () => {
      while (true) {
        const channelIndex = nextChannel;
        nextChannel += 1;
        if (channelIndex >= channelInputs.length) return;
        results[channelIndex] = await this.runChannel(channelInputs[channelIndex], channelIndex);
      }
    });
    await Promise.all(runners);
    return results;
  }

  private async runChannel(input: SparseTsdfInput, channelIndex: number): Promise<SparseTsdfMesh> {
    const requestId = this.nextRequestId++;
    const worker = new SparseTsdfChannelWorker();
    return this.runOneShotWorker<ChannelResultMessage, SparseTsdfMesh>(
      worker,
      (message) =>
        message?.type === 'channel-result' &&
        message.requestId === requestId &&
        message.channelIndex === channelIndex,
      (message) => message.result,
      () => worker.postMessage(
        { type: 'reconstruct-channel', requestId, channelIndex, input },
        inputTransfers(input),
      ),
    );
  }

  private async merge(
    summary: DirectionalTsdfPlanSummary,
    meshes: SparseTsdfMesh[],
  ): Promise<DirectionalTsdfMesh> {
    const requestId = this.nextRequestId++;
    const worker = new DirectionalTsdfMergeWorker();
    const transfer: Transferable[] = [];
    const seen = new Set<ArrayBuffer>();
    for (const mesh of meshes) addTransferBuffer(transfer, seen, mesh.trianglePositions.buffer);
    const channelComputeMs = meshes.reduce((sum, mesh) => sum + mesh.computeMs, 0);
    return this.runOneShotWorker<MergeResultMessage, DirectionalTsdfMesh>(
      worker,
      (message) => message?.type === 'merge-result' && message.requestId === requestId,
      (message) => message.result,
      () => worker.postMessage({
        type: 'merge',
        requestId,
        summary,
        meshes,
        totalComputeMs: channelComputeMs,
      }, transfer),
    );
  }

  private runOneShotWorker<TMessage, TResult>(
    worker: Worker,
    accepts: (message: TMessage) => boolean,
    project: (message: TMessage) => TResult,
    start: () => void,
  ): Promise<TResult> {
    this.activeWorkers.add(worker);
    return new Promise<TResult>((resolve, reject) => {
      let settled = false;
      const cleanup = (): void => {
        worker.removeEventListener('message', handleMessage as EventListener);
        worker.removeEventListener('error', handleError as EventListener);
        this.activeWorkers.delete(worker);
        this.pendingRejectors.delete(fail);
        worker.terminate();
      };
      const fail = (reason: unknown): void => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(reason);
      };
      const handleMessage = (event: MessageEvent<TMessage>): void => {
        if (settled || !accepts(event.data)) return;
        settled = true;
        const result = project(event.data);
        cleanup();
        resolve(result);
      };
      const handleError = (event: ErrorEvent): void => {
        fail(event.error ?? new Error(event.message));
      };
      this.pendingRejectors.add(fail);
      worker.addEventListener('message', handleMessage as EventListener);
      worker.addEventListener('error', handleError as EventListener);
      try { start(); }
      catch (error) { fail(error); }
    });
  }
}
