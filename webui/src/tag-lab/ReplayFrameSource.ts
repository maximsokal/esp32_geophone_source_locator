import patternData from '../../../tools/apriltag_sheet/tag_patterns.gen.json';
import { scheduleVideoFrames, type FrameSource } from './frameSource';
import type { LabSourceType } from './TagLabDiagnostics';

export type ReplayKind = 'image' | 'video' | 'synthetic';

interface Pattern { tag_id: number; modules: number[][]; }
const PATTERNS = (patternData as { tags: Pattern[] }).tags;

/** Deterministic animated scene of the five sheet tags (moving, rotating, appearing and disappearing). */
export function drawSyntheticScene(ctx: CanvasRenderingContext2D, tMs: number, width: number, height: number): number[] {
  ctx.fillStyle = '#d9d9d9';
  ctx.fillRect(0, 0, width, height);
  const visible: number[] = [];
  PATTERNS.forEach((pattern, index) => {
    const phase = tMs / 1000 + index * 1.3;
    const on = Math.sin(phase * 0.55) > -0.35;
    if (!on) return;
    visible.push(pattern.tag_id);
    const cell = 12 + 4 * Math.sin(phase * 0.4 + index);
    const size = cell * 8;
    const cx = width * (0.15 + 0.17 * index) + 30 * Math.sin(phase * 0.7);
    const cy = height * (0.5 + 0.22 * Math.sin(phase * 0.3 + index * 2));
    ctx.save();
    ctx.translate(cx, cy);
    ctx.rotate(0.5 * Math.sin(phase * 0.45));
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(-size / 2 - cell, -size / 2 - cell, size + 2 * cell, size + 2 * cell);
    ctx.fillStyle = '#000000';
    for (let row = 0; row < 8; row += 1) for (let col = 0; col < 8; col += 1) {
      if (pattern.modules[row][col]) ctx.fillRect(-size / 2 + col * cell, -size / 2 + row * cell, cell + 0.5, cell + 0.5);
    }
    ctx.restore();
  });
  return visible;
}

export class ReplayFrameSource implements FrameSource {
  readonly type: LabSourceType;
  readonly element: HTMLVideoElement | HTMLCanvasElement | HTMLImageElement;
  private objectUrl: string | null = null;
  private readonly cancelers = new Set<() => void>();
  private timer: ReturnType<typeof setInterval> | null = null;

  private constructor(readonly kind: ReplayKind, element: HTMLVideoElement | HTMLCanvasElement | HTMLImageElement) {
    this.type = kind;
    this.element = element;
  }

  static synthetic(canvas: HTMLCanvasElement, width = 1280, height = 720): ReplayFrameSource {
    canvas.width = width; canvas.height = height;
    const source = new ReplayFrameSource('synthetic', canvas);
    const ctx = canvas.getContext('2d');
    if (ctx !== null) {
      const t0 = performance.now();
      source.timer = setInterval(() => drawSyntheticScene(ctx, performance.now() - t0, width, height), 33);
      drawSyntheticScene(ctx, 0, width, height);
    }
    return source;
  }

  static async fromImageFile(image: HTMLImageElement, file: Blob): Promise<ReplayFrameSource> {
    const source = new ReplayFrameSource('image', image);
    source.objectUrl = URL.createObjectURL(file);
    image.src = source.objectUrl;
    await image.decode();
    return source;
  }

  static async fromVideoFile(video: HTMLVideoElement, file: Blob): Promise<ReplayFrameSource> {
    const source = new ReplayFrameSource('video', video);
    source.objectUrl = URL.createObjectURL(file);
    video.srcObject = null;
    video.src = source.objectUrl;
    video.loop = true; video.muted = true; video.playsInline = true;
    await video.play();
    return source;
  }

  size(): { width: number; height: number } | null {
    const el = this.element;
    if (el instanceof HTMLVideoElement) return el.videoWidth > 0 ? { width: el.videoWidth, height: el.videoHeight } : null;
    if (el instanceof HTMLImageElement) return el.naturalWidth > 0 ? { width: el.naturalWidth, height: el.naturalHeight } : null;
    return { width: el.width, height: el.height };
  }

  onFrame(callback: (nowMs: number) => void): () => void {
    let cancel: () => void;
    if (this.element instanceof HTMLVideoElement) {
      cancel = scheduleVideoFrames(this.element, callback);
    } else {
      let raf = 0; let stopped = false; let last = 0;
      const loop = (now: number): void => {
        if (stopped) return;
        if (now - last >= 33) { last = now; callback(now); }
        raf = requestAnimationFrame(loop);
      };
      raf = requestAnimationFrame(loop);
      cancel = () => { stopped = true; cancelAnimationFrame(raf); };
    }
    const wrapped = (): void => { cancel(); this.cancelers.delete(wrapped); };
    this.cancelers.add(wrapped);
    return wrapped;
  }

  stop(): void {
    for (const cancel of [...this.cancelers]) cancel();
    if (this.timer !== null) { clearInterval(this.timer); this.timer = null; }
    if (this.element instanceof HTMLVideoElement) { this.element.pause(); this.element.removeAttribute('src'); this.element.load(); }
    if (this.objectUrl !== null) { URL.revokeObjectURL(this.objectUrl); this.objectUrl = null; }
  }
}
