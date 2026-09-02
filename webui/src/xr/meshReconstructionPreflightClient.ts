import MeshReconstructionPreflightWorker from './meshReconstructionPreflight.worker?worker&inline';
import type { MeshReconstructionPreflight } from './meshReconstructionPreflight';

export interface MeshPreflightOwnedArrays {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
}

export interface MeshPreflightWorkerResult {
  arrays: MeshPreflightOwnedArrays;
  preflight: MeshReconstructionPreflight | null;
  computeMs: number;
  error: string | null;
}

interface PreflightSuccessMessage {
  type: 'preflight-result';
  requestId: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  result: MeshReconstructionPreflight;
  computeMs: number;
}

interface PreflightFailureMessage {
  type: 'preflight-error';
  requestId: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  computeMs: number;
  error: string;
}

type PreflightResultMessage = PreflightSuccessMessage | PreflightFailureMessage;

/**
 * One-shot zero-copy component preflight. The caller temporarily transfers
 * ownership of the three surfel arrays and receives the same buffers back with
 * either the analysis or a recoverable computation error. This keeps the UI
 * thread free without cloning a large cloud and still permits synchronous
 * fail-safe fallback after a worker-side exception.
 */
export class MeshReconstructionPreflightClient {
  private nextRequestId = 1;
  private generation = 0;
  private worker: Worker | null = null;
  private rejectPending: ((reason: unknown) => void) | null = null;

  constructor(
    private readonly workerFactory: () => Worker = () => new MeshReconstructionPreflightWorker(),
  ) {}

  analyze(arrays: MeshPreflightOwnedArrays): Promise<MeshPreflightWorkerResult> {
    this.reset('Mesh reconstruction preflight superseded by a newer request.');
    const generation = this.generation;
    const requestId = this.nextRequestId++;
    const worker = this.workerFactory();
    this.worker = worker;

    return new Promise<MeshPreflightWorkerResult>((resolve, reject) => {
      let settled = false;
      const cleanup = (): void => {
        worker.removeEventListener('message', handleMessage as EventListener);
        worker.removeEventListener('error', handleError as EventListener);
        if (this.worker === worker) this.worker = null;
        if (this.rejectPending === fail) this.rejectPending = null;
        worker.terminate();
      };
      const fail = (reason: unknown): void => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(reason);
      };
      const handleMessage = (event: MessageEvent<PreflightResultMessage>): void => {
        const message = event.data;
        if (
          settled || generation !== this.generation ||
          (message?.type !== 'preflight-result' && message?.type !== 'preflight-error') ||
          message.requestId !== requestId
        ) return;
        settled = true;
        cleanup();
        resolve({
          arrays: {
            positions: message.positions,
            normals: message.normals,
            confidence: message.confidence,
            voxelSizeMeters: arrays.voxelSizeMeters,
          },
          preflight: message.type === 'preflight-result' ? message.result : null,
          computeMs: message.computeMs,
          error: message.type === 'preflight-error' ? message.error : null,
        });
      };
      const handleError = (event: ErrorEvent): void => {
        // A catastrophic worker/runtime failure cannot return transferred buffers.
        // Reject explicitly; normal algorithm exceptions use preflight-error and
        // return ownership before the caller decides whether to fall back.
        fail(event.error ?? new Error(event.message));
      };

      this.rejectPending = fail;
      worker.addEventListener('message', handleMessage as EventListener);
      worker.addEventListener('error', handleError as EventListener);
      try {
        worker.postMessage({
          type: 'preflight',
          requestId,
          positions: arrays.positions,
          normals: arrays.normals,
          confidence: arrays.confidence,
          voxelSizeMeters: arrays.voxelSizeMeters,
        }, [arrays.positions.buffer, arrays.normals.buffer, arrays.confidence.buffer]);
      } catch (error) {
        fail(error);
      }
    });
  }

  reset(reason = 'Mesh reconstruction preflight reset.'): void {
    this.generation += 1;
    this.rejectPending?.(new Error(reason));
    this.rejectPending = null;
    this.worker?.terminate();
    this.worker = null;
  }

  dispose(): void {
    this.reset('Mesh reconstruction preflight worker disposed.');
  }
}
