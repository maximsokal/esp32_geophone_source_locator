import TagDetectorWorker from './tagDetector.worker.ts?worker&inline';
import type { DetectedAprilTag } from './tagDetector';

export interface TagCameraFrame {
  width: number;
  height: number;
  rgba: Uint8Array;
  capturedAtMs: number;
  projectionMatrix: ArrayLike<number>;
  worldFromView: ArrayLike<number>;
  /** The RGBA buffer is a fresh, top-left-origin readback owned by this frame. */
  rgbaOwnership?: 'owned';
}

export interface DetectedAprilTagFrame {
  width: number;
  height: number;
  capturedAtMs: number;
  detections: DetectedAprilTag[];
  projectionMatrix: Float32Array;
  worldFromView: Float32Array;
}

type WorkerResult = { type: 'result'; requestId: number; epoch: number; detections: DetectedAprilTag[] };
type WorkerLike = Pick<Worker, 'addEventListener' | 'removeEventListener' | 'postMessage' | 'terminate'>;
interface Pending {
  epoch: number; width: number; height: number; capturedAtMs: number;
  projectionMatrix: Float32Array; worldFromView: Float32Array;
  resolve: (frame: DetectedAprilTagFrame | null) => void;
  reject: (error: Error) => void;
}

export class TagDetectorClient {
  private worker: WorkerLike | null = null;
  private epoch = 1;
  private requestId = 0;
  private pending = new Map<number, Pending>();
  private disposed = false;
  private readonly factory: () => WorkerLike;

  constructor(factory: () => WorkerLike = () => new TagDetectorWorker()) {
    this.factory = factory;
    try {
      this.worker = factory();
      this.worker.addEventListener('message', this.onMessage);
      this.worker.addEventListener('error', this.onError);
      this.worker.postMessage({ type: 'reset', epoch: this.epoch });
    } catch { this.worker = null; }
  }

  get busy(): boolean { return this.pending.size > 0; }
  get workerBacked(): boolean { return this.worker !== null; }

  detect(frame: TagCameraFrame): Promise<DetectedAprilTagFrame | null> {
    if (this.disposed) return Promise.reject(new Error('Tag detector client is disposed.'));
    if (this.worker === null) return Promise.reject(new Error('Tag detector worker is unavailable.'));
    if (this.pending.size > 0) return Promise.resolve(null);
    const requestId = ++this.requestId;
    const backing = frame.rgba.buffer;
    const canTransferOwnedRgba = frame.rgbaOwnership === 'owned' && backing instanceof ArrayBuffer &&
      frame.rgba.byteOffset === 0 && frame.rgba.byteLength === backing.byteLength;
    const rgba = canTransferOwnedRgba ? frame.rgba : frame.rgba.slice();
    return new Promise((resolve, reject) => {
      this.pending.set(requestId, {
        epoch: this.epoch, width: frame.width, height: frame.height, capturedAtMs: frame.capturedAtMs,
        projectionMatrix: Float32Array.from(frame.projectionMatrix),
        worldFromView: Float32Array.from(frame.worldFromView), resolve, reject,
      });
      this.worker!.postMessage({ type: 'detect', requestId, epoch: this.epoch, width: frame.width, height: frame.height, capturedAtMs: frame.capturedAtMs, rgba }, [rgba.buffer]);
    });
  }

  reset(): void {
    this.epoch += 1;
    for (const item of this.pending.values()) item.reject(new Error('Tag detector reset for a new XR session.'));
    this.pending.clear();
    this.worker?.postMessage({ type: 'reset', epoch: this.epoch });
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.epoch += 1;
    for (const item of this.pending.values()) item.reject(new Error('Tag detector client disposed.'));
    this.pending.clear();
    this.detachWorker();
  }

  private readonly onMessage = (event: MessageEvent<WorkerResult>): void => {
    const message = event.data;
    const request = this.pending.get(message.requestId);
    if (message.type !== 'result' || request === undefined) return;
    this.pending.delete(message.requestId);
    if (request.epoch !== this.epoch || message.epoch !== request.epoch) {
      request.reject(new Error('Tag detector result belongs to an obsolete XR session.'));
      return;
    }
    request.resolve({
      width: request.width, height: request.height, capturedAtMs: request.capturedAtMs,
      detections: message.detections, projectionMatrix: request.projectionMatrix, worldFromView: request.worldFromView,
    });
  };

  private readonly onError = (event: ErrorEvent): void => {
    const error = new Error(event.message || 'Tag detector worker failed.');
    for (const item of this.pending.values()) item.reject(error);
    this.pending.clear();
    this.detachWorker();
  };

  private detachWorker(): void {
    if (this.worker === null) return;
    this.worker.removeEventListener('message', this.onMessage);
    this.worker.removeEventListener('error', this.onError);
    this.worker.terminate();
    this.worker = null;
  }
}
