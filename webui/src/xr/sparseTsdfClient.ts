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

export class SparseTsdfMeshClient {
  private readonly worker = new SparseTsdfWorker();
  private nextRequestId = 1;
  private epoch = 0;
  private pending: Pending | null = null;

  constructor() {
    this.worker.addEventListener('message', this.handleMessage);
    this.worker.addEventListener('error', this.handleError);
  }

  reconstruct(input: SparseTsdfReconstructInput): Promise<SparseTsdfMesh> {
    this.resetPending('Sparse TSDF reconstruction superseded by a newer scan.');
    const id = this.nextRequestId++;
    const epoch = this.epoch;
    return new Promise((resolve, reject) => {
      this.pending = { id, epoch, resolve, reject };
      const transfer: Transferable[] = [
        input.positions.buffer,
        input.normals.buffer,
        input.confidence.buffer,
      ];
      if (input.freeSpacePositions !== undefined) transfer.push(input.freeSpacePositions.buffer);
      this.worker.postMessage({
        type: 'reconstruct',
        requestId: id,
        positions: input.positions,
        normals: input.normals,
        confidence: input.confidence,
        voxelSizeMeters: input.voxelSizeMeters,
        freeSpacePositions: input.freeSpacePositions,
        freeSpaceVoxelSizeMeters: input.freeSpaceVoxelSizeMeters,
        maxInputPoints: input.maxInputPoints,
        maxTriangles: input.maxTriangles,
      }, transfer);
    });
  }

  reset(): void {
    this.epoch += 1;
    this.resetPending('Sparse TSDF reconstruction reset.');
    this.worker.postMessage({ type: 'reset', epoch: this.epoch });
  }

  dispose(): void {
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
