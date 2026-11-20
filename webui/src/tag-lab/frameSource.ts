import type { LabFrame } from './labPipeline';
import type { LabSourceType } from './TagLabDiagnostics';

export interface FrameSourceInfo { label: string; width: number; height: number; }

/** A live image source shown in the page and sampled by the lab. */
export interface FrameSource {
  readonly type: LabSourceType;
  /** Element displayed under the overlay; its intrinsic size is the analysed frame size before scaling. */
  readonly element: HTMLVideoElement | HTMLCanvasElement | HTMLImageElement;
  /** Intrinsic pixel size of the current image, or null before the first frame. */
  size(): { width: number; height: number } | null;
  /** Called for every new frame opportunity; returns an unsubscribe. */
  onFrame(callback: (nowMs: number) => void): () => void;
  stop(): void;
}

/**
 * Prefer requestVideoFrameCallback (fires once per decoded video frame); fall back to requestAnimationFrame,
 * which is invoked even when no new camera frame arrived — callers must tolerate repeated frames.
 */
export function scheduleVideoFrames(video: HTMLVideoElement, callback: (nowMs: number) => void): () => void {
  let cancelled = false;
  if (typeof video.requestVideoFrameCallback === 'function') {
    let handle = 0;
    const step = (now: number): void => {
      if (cancelled) return;
      callback(now);
      handle = video.requestVideoFrameCallback(step);
    };
    handle = video.requestVideoFrameCallback(step);
    return () => { cancelled = true; video.cancelVideoFrameCallback?.(handle); };
  }
  let raf = 0;
  const loop = (now: number): void => {
    if (cancelled) return;
    callback(now);
    raf = requestAnimationFrame(loop);
  };
  raf = requestAnimationFrame(loop);
  return () => { cancelled = true; cancelAnimationFrame(raf); };
}

/** Resamples any drawable element to a fixed analysis size and returns an OWNED top-left RGBA frame. */
export class FrameGrabber {
  private canvas: HTMLCanvasElement | OffscreenCanvas | null = null;
  private context: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D | null = null;
  private frameId = 0;
  constructor(private readonly createCanvas: (w: number, h: number) => HTMLCanvasElement | OffscreenCanvas = (w, h) => {
    if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
    const element = document.createElement('canvas'); element.width = w; element.height = h; return element;
  }) {}

  grab(source: CanvasImageSource, targetWidth: number, targetHeight: number, type: LabSourceType, sessionEpoch: number, nowMs: number): LabFrame | null {
    if (targetWidth <= 0 || targetHeight <= 0) return null;
    if (this.canvas === null || this.canvas.width !== targetWidth || this.canvas.height !== targetHeight) {
      this.canvas = this.createCanvas(targetWidth, targetHeight);
      this.context = this.canvas.getContext('2d', { willReadFrequently: true }) as CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D | null;
    }
    if (this.context === null) return null;
    this.context.drawImage(source, 0, 0, targetWidth, targetHeight);
    const image = this.context.getImageData(0, 0, targetWidth, targetHeight);
    this.frameId += 1;
    return {
      frameId: this.frameId, sourceType: type, capturedAtMs: nowMs, width: targetWidth, height: targetHeight,
      rgba: new Uint8Array(image.data.buffer, image.data.byteOffset, image.data.byteLength), sessionEpoch,
    };
  }
}
