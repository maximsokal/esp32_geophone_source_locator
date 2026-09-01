import VisualLineEvidenceWorker from './visualLineEvidence.worker.ts?worker&inline';
import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';
import { detectVisualLinesNow, type VisualEvidenceFrame, type VisualJunction, type VisualLineSegment } from './visualLineEvidence';

interface WorkerResultMessage {
  type: 'result';
  requestId: number;
  epoch: number;
  lines: VisualLineSegment[];
  junctions: VisualJunction[];
}

interface PendingRequest {
  epoch: number;
  keyframeId: number;
  capturedAtMs: number;
  projectionMatrix: Float32Array;
  worldFromView: Float32Array;
  resolve: (value: VisualEvidenceFrame) => void;
  reject: (reason: Error) => void;
}

function cloneMatrices(
  projectionMatrix: ArrayLike<number>,
  worldFromView: ArrayLike<number>,
): { projectionMatrix: Float32Array; worldFromView: Float32Array } {
  return {
    projectionMatrix: Float32Array.from(projectionMatrix),
    worldFromView: Float32Array.from(worldFromView),
  };
}

export class VisualLineEvidenceClient {
  private worker: Worker | null = null;
  private requestId = 0;
  private epoch = 1;
  private pending = new Map<number, PendingRequest>();
  private fallbackInFlight = 0;
  private disposed = false;

  constructor() {
    if (typeof Worker === 'undefined') return;
    try {
      // Inline Vite worker keeps ESP deployment to the existing one-JS/one-CSS
      // contract. No hashed worker asset or extra HTTPS URI is required.
      this.worker = new VisualLineEvidenceWorker();
      this.worker.addEventListener('message', this.handleWorkerMessage);
      this.worker.addEventListener('error', this.handleWorkerError);
      this.worker.postMessage({ type: 'reset', epoch: this.epoch });
    } catch {
      this.worker = null;
    }
  }

  get busy(): boolean { return this.pending.size > 0 || this.fallbackInFlight > 0; }
  get workerBacked(): boolean { return this.worker !== null; }

  reset(): void {
    this.epoch += 1;
    for (const request of this.pending.values()) request.reject(new Error('Visual evidence client reset for a new scan.'));
    this.pending.clear();
    // A fallback task already queued on the event loop is allowed to finish but
    // remains counted as busy and will reject itself by epoch. This prevents a
    // new scan from stacking another main-thread Hough job on top of it.
    this.worker?.postMessage({ type: 'reset', epoch: this.epoch });
  }

  analyze(
    frame: RgbDepthGuidanceFrame,
    keyframeId: number,
    projectionMatrix: ArrayLike<number>,
    worldFromView: ArrayLike<number>,
  ): Promise<VisualEvidenceFrame> {
    if (this.disposed) return Promise.reject(new Error('Visual evidence client is disposed.'));
    const matrices = cloneMatrices(projectionMatrix, worldFromView);

    if (this.worker === null) {
      const requestEpoch = this.epoch;
      this.fallbackInFlight += 1;
      return new Promise<VisualEvidenceFrame>((resolve, reject) => {
        setTimeout(() => {
          try {
            if (this.disposed || requestEpoch !== this.epoch) {
              reject(new Error('Visual evidence fallback belongs to an obsolete scan epoch.'));
              return;
            }
            const detected = detectVisualLinesNow(frame);
            if (this.disposed || requestEpoch !== this.epoch) {
              reject(new Error('Visual evidence fallback belongs to an obsolete scan epoch.'));
              return;
            }
            resolve({
              keyframeId,
              capturedAtMs: frame.capturedAtMs,
              lines: detected.lines,
              junctions: detected.junctions,
              projectionMatrix: matrices.projectionMatrix,
              worldFromView: matrices.worldFromView,
            });
          } catch (error) {
            reject(error instanceof Error ? error : new Error(String(error)));
          } finally {
            this.fallbackInFlight = Math.max(0, this.fallbackInFlight - 1);
          }
        }, 0);
      });
    }

    const requestId = ++this.requestId;
    return new Promise<VisualEvidenceFrame>((resolve, reject) => {
      this.pending.set(requestId, {
        epoch: this.epoch,
        keyframeId,
        capturedAtMs: frame.capturedAtMs,
        projectionMatrix: matrices.projectionMatrix,
        worldFromView: matrices.worldFromView,
        resolve,
        reject,
      });
      const edgeStrength = frame.edgeStrength.slice();
      const gradientX = frame.gradientX.slice();
      const gradientY = frame.gradientY.slice();
      const transfer: Transferable[] = [
        edgeStrength.buffer as ArrayBuffer,
        gradientX.buffer as ArrayBuffer,
        gradientY.buffer as ArrayBuffer,
      ];
      this.worker!.postMessage({
        type: 'analyze',
        requestId,
        epoch: this.epoch,
        width: frame.width,
        height: frame.height,
        capturedAtMs: frame.capturedAtMs,
        edgeStrength,
        gradientX,
        gradientY,
      }, transfer);
    });
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.epoch += 1;
    for (const request of this.pending.values()) request.reject(new Error('Visual evidence client disposed.'));
    this.pending.clear();
    this.detachBrokenWorker();
  }

  private detachBrokenWorker(): void {
    if (this.worker === null) return;
    this.worker.removeEventListener('message', this.handleWorkerMessage);
    this.worker.removeEventListener('error', this.handleWorkerError);
    this.worker.terminate();
    this.worker = null;
  }

  private readonly handleWorkerMessage = (event: MessageEvent<WorkerResultMessage>): void => {
    const message = event.data;
    if (message.type !== 'result') return;
    const request = this.pending.get(message.requestId);
    if (request === undefined) return;
    this.pending.delete(message.requestId);
    if (message.epoch !== request.epoch || request.epoch !== this.epoch) {
      request.reject(new Error('Visual evidence result belongs to an obsolete scan epoch.'));
      return;
    }
    request.resolve({
      keyframeId: request.keyframeId,
      capturedAtMs: request.capturedAtMs,
      lines: message.lines,
      junctions: message.junctions,
      projectionMatrix: request.projectionMatrix,
      worldFromView: request.worldFromView,
    });
  };

  private readonly handleWorkerError = (event: ErrorEvent): void => {
    const error = new Error(event.message || 'Visual evidence worker failed.');
    for (const request of this.pending.values()) request.reject(error);
    this.pending.clear();
    // Subsequent analyses use the bounded async fallback instead of continuing
    // to post into a worker that has already raised an uncaught error.
    this.detachBrokenWorker();
  };
}
