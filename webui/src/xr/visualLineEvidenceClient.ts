import { deviceComputeProfilePromise } from './deviceComputeProfile';
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

export interface VisualLineEvidenceClientOptions {
  /** Keep isolated tests/offline tools compatible; production XR sets false. */
  allowMainThreadFallback?: boolean;
  /** Production XR waits for the pre-scan compute profile before allocating. */
  deferWorkerUntilAnalyze?: boolean;
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
  private workerStartupInFlight = 0;
  private disposed = false;
  private readonly allowMainThreadFallback: boolean;
  private readonly deferWorkerUntilAnalyze: boolean;

  constructor(options: VisualLineEvidenceClientOptions = {}) {
    this.allowMainThreadFallback = options.allowMainThreadFallback ?? true;
    this.deferWorkerUntilAnalyze = options.deferWorkerUntilAnalyze ?? false;
    if (!this.deferWorkerUntilAnalyze) this.tryCreateWorker();
  }

  private tryCreateWorker(): boolean {
    if (this.disposed || this.worker !== null || typeof Worker === 'undefined') return this.worker !== null;
    try {
      // Inline Vite worker keeps ESP deployment to the existing one-JS/one-CSS
      // contract. No hashed worker asset or extra HTTPS URI is required.
      this.worker = new VisualLineEvidenceWorker();
      this.worker.addEventListener('message', this.handleWorkerMessage);
      this.worker.addEventListener('error', this.handleWorkerError);
      this.worker.postMessage({ type: 'reset', epoch: this.epoch });
      return true;
    } catch {
      this.worker = null;
      return false;
    }
  }

  get busy(): boolean { return this.pending.size > 0 || this.fallbackInFlight > 0 || this.workerStartupInFlight > 0; }
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

  private analyzeFallback(
    frame: RgbDepthGuidanceFrame,
    keyframeId: number,
    projectionMatrix: ArrayLike<number>,
    worldFromView: ArrayLike<number>,
  ): Promise<VisualEvidenceFrame> {
    if (!this.allowMainThreadFallback) {
      return Promise.reject(new Error('Visual evidence requires a Dedicated Worker during live XR.'));
    }
    const matrices = cloneMatrices(projectionMatrix, worldFromView);
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

  analyze(
    frame: RgbDepthGuidanceFrame,
    keyframeId: number,
    projectionMatrix: ArrayLike<number>,
    worldFromView: ArrayLike<number>,
  ): Promise<VisualEvidenceFrame> {
    if (this.disposed) return Promise.reject(new Error('Visual evidence client is disposed.'));
    if (this.worker === null && this.deferWorkerUntilAnalyze) {
      const requestEpoch = this.epoch;
      this.workerStartupInFlight += 1;
      return deviceComputeProfilePromise.then((compute) => {
        if (this.disposed || requestEpoch !== this.epoch) {
          throw new Error('Visual evidence worker startup belongs to an obsolete scan epoch.');
        }
        if (compute.liveWorkerBudget >= 1 && this.tryCreateWorker()) {
          return this.analyze(frame, keyframeId, projectionMatrix, worldFromView);
        }
        return this.analyzeFallback(frame, keyframeId, projectionMatrix, worldFromView);
      }).finally(() => {
        this.workerStartupInFlight = Math.max(0, this.workerStartupInFlight - 1);
      });
    }

    if (this.worker === null) {
      return this.analyzeFallback(frame, keyframeId, projectionMatrix, worldFromView);
    }

    const matrices = cloneMatrices(projectionMatrix, worldFromView);
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
      this.worker.postMessage({
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
    // A broken live worker must never silently migrate a heavy Hough pass back to
    // the XR main thread. Offline/test clients can still opt into the fallback.
    this.detachBrokenWorker();
  };
}
