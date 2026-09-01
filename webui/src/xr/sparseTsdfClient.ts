import { clientDiagnostics } from '../data/clientDiagnostics';
import { optionalMonocularPrior } from './monocularPriorRuntime';
import SparseTsdfWorker from './sparseTsdf.worker?worker&inline';
import type { SparseTsdfMesh } from './sparseTsdf';

export interface SparseTsdfReconstructInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
  maxInputPoints?: number;
  maxTriangles?: number;
}

interface ResultMessage {
  type: 'result';
  requestId: number;
  epoch: number;
  result: SparseTsdfMesh;
}

interface Pending {
  id: number;
  epoch: number;
  resolve: (result: SparseTsdfMesh) => void;
  reject: (reason?: unknown) => void;
}

const MAX_AI_REJECT_FRACTION = 0.20;
const MIN_POST_AI_SURFELS = 60;

function filterWithCalibratedPostScanPrior(
  input: SparseTsdfReconstructInput,
): { input: SparseTsdfReconstructInput; rejected: number; applied: boolean } {
  const pointCount = Math.min(
    Math.floor(input.positions.length / 3),
    Math.floor(input.normals.length / 3),
    input.confidence.length,
  );
  if (pointCount < MIN_POST_AI_SURFELS || optionalMonocularPrior.calibrationConfidence <= 0) {
    return { input, rejected: 0, applied: false };
  }

  const accepted: number[] = [];
  for (let index = 0; index < pointCount; index += 1) {
    const offset = index * 3;
    const agreement = optionalMonocularPrior.postScanSurfaceAgreement([
      input.positions[offset],
      input.positions[offset + 1],
      input.positions[offset + 2],
    ]);
    if (!agreement.available || agreement.accepted) accepted.push(index);
  }

  const rejected = pointCount - accepted.length;
  if (rejected === 0) return { input, rejected: 0, applied: true };
  const rejectFraction = rejected / pointCount;
  if (rejectFraction > MAX_AI_REJECT_FRACTION || accepted.length < MIN_POST_AI_SURFELS) {
    clientDiagnostics.warn('SCAN_AI_POST_SCAN_FAIL_OPEN', {
      sourcePoints: pointCount,
      proposedRejected: rejected,
      rejectFraction: Number(rejectFraction.toFixed(3)),
      calibration: Number(optionalMonocularPrior.calibrationConfidence.toFixed(3)),
      calibrationModel: optionalMonocularPrior.calibrationModel,
    });
    return { input, rejected: 0, applied: false };
  }

  const positions = new Float32Array(accepted.length * 3);
  const normals = new Float32Array(accepted.length * 3);
  const confidence = new Float32Array(accepted.length);
  accepted.forEach((source, target) => {
    const sourceOffset = source * 3;
    const targetOffset = target * 3;
    positions[targetOffset] = input.positions[sourceOffset];
    positions[targetOffset + 1] = input.positions[sourceOffset + 1];
    positions[targetOffset + 2] = input.positions[sourceOffset + 2];
    normals[targetOffset] = input.normals[sourceOffset];
    normals[targetOffset + 1] = input.normals[sourceOffset + 1];
    normals[targetOffset + 2] = input.normals[sourceOffset + 2];
    confidence[target] = input.confidence[source];
  });

  return {
    input: { ...input, positions, normals, confidence, maxInputPoints: Math.min(input.maxInputPoints ?? accepted.length, accepted.length) },
    rejected,
    applied: true,
  };
}

export class SparseTsdfMeshClient {
  private readonly worker = new SparseTsdfWorker();
  private nextRequestId = 1;
  private epoch = 0;
  private generation = 0;
  private pending: Pending | null = null;

  constructor() {
    this.worker.addEventListener('message', this.handleMessage);
    this.worker.addEventListener('error', this.handleError);
  }

  async reconstruct(input: SparseTsdfReconstructInput): Promise<SparseTsdfMesh> {
    this.resetPending('Sparse TSDF reconstruction superseded by a newer scan.');
    const generation = ++this.generation;
    const epoch = this.epoch;
    let reconstructionInput = input;

    if (optionalMonocularPrior.isEnabled && optionalMonocularPrior.hasStagedPostScanFrame) {
      const started = performance.now();
      clientDiagnostics.info('SCAN_AI_POST_SCAN_START', {
        sourcePoints: Math.floor(input.positions.length / 3),
        model: 'onnx-community/depth-anything-v2-small',
      });
      const calibrated = await optionalMonocularPrior.runStagedPostScan();
      if (generation !== this.generation || epoch !== this.epoch) {
        throw new Error('Sparse TSDF reconstruction superseded during post-scan AI refinement.');
      }
      const filtered = calibrated
        ? filterWithCalibratedPostScanPrior(input)
        : { input, rejected: 0, applied: false };
      reconstructionInput = filtered.input;
      clientDiagnostics.info('SCAN_AI_POST_SCAN_READY', {
        calibrated,
        applied: filtered.applied,
        calibration: Number(optionalMonocularPrior.calibrationConfidence.toFixed(3)),
        calibrationModel: optionalMonocularPrior.calibrationModel,
        calibrationDiagnostics: optionalMonocularPrior.calibrationDiagnostics,
        sourcePoints: Math.floor(input.positions.length / 3),
        rejectedPoints: filtered.rejected,
        retainedPoints: Math.floor(reconstructionInput.positions.length / 3),
        computeMs: Number((performance.now() - started).toFixed(1)),
        error: optionalMonocularPrior.error,
      });
      void clientDiagnostics.flush();
    }

    if (generation !== this.generation || epoch !== this.epoch) {
      throw new Error('Sparse TSDF reconstruction superseded before worker dispatch.');
    }

    const id = this.nextRequestId++;
    return new Promise((resolve, reject) => {
      this.pending = { id, epoch, resolve, reject };
      const transfer: Transferable[] = [
        reconstructionInput.positions.buffer,
        reconstructionInput.normals.buffer,
        reconstructionInput.confidence.buffer,
      ];
      if (reconstructionInput.freeSpacePositions !== undefined) transfer.push(reconstructionInput.freeSpacePositions.buffer);
      this.worker.postMessage({
        type: 'reconstruct',
        requestId: id,
        positions: reconstructionInput.positions,
        normals: reconstructionInput.normals,
        confidence: reconstructionInput.confidence,
        voxelSizeMeters: reconstructionInput.voxelSizeMeters,
        freeSpacePositions: reconstructionInput.freeSpacePositions,
        freeSpaceVoxelSizeMeters: reconstructionInput.freeSpaceVoxelSizeMeters,
        maxInputPoints: reconstructionInput.maxInputPoints,
        maxTriangles: reconstructionInput.maxTriangles,
      }, transfer);
    });
  }

  reset(): void {
    this.generation += 1;
    this.epoch += 1;
    this.resetPending('Sparse TSDF reconstruction reset.');
    this.worker.postMessage({ type: 'reset', epoch: this.epoch });
  }

  dispose(): void {
    this.generation += 1;
    this.resetPending('Sparse TSDF worker disposed.');
    this.worker.removeEventListener('message', this.handleMessage);
    this.worker.removeEventListener('error', this.handleError);
    this.worker.terminate();
  }

  private resetPending(message: string): void {
    if (this.pending === null) return;
    this.pending.reject(new Error(message));
    this.pending = null;
  }

  private readonly handleMessage = (event: MessageEvent<ResultMessage>): void => {
    const message = event.data;
    const pending = this.pending;
    if (
      message?.type !== 'result' || pending === null ||
      message.requestId !== pending.id || message.epoch !== pending.epoch || message.epoch !== this.epoch
    ) return;
    this.pending = null;
    pending.resolve(message.result);
  };

  private readonly handleError = (event: ErrorEvent): void => {
    if (this.pending === null) return;
    const pending = this.pending;
    this.pending = null;
    pending.reject(event.error ?? new Error(event.message));
  };
}
