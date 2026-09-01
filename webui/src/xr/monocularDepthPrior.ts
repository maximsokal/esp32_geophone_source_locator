export interface MonocularDepthPriorFrame {
  width: number;
  height: number;
  relativeDepth: Float32Array;
  capturedAtMs: number;
  source: string;
}

export interface MetricDepthAnchor {
  u: number;
  v: number;
  depthMeters: number;
  weight?: number;
}

export interface LogAffineDepthCalibration {
  scale: number;
  bias: number;
  inliers: number;
  rmsLogError: number;
  confidence: number;
}

export type MonocularDepthProviderStatus = 'disabled' | 'unavailable' | 'loading' | 'ready' | 'error';

export interface MonocularDepthProvider {
  readonly name: string;
  readonly status: MonocularDepthProviderStatus;
  readonly error: string | null;
  isSupported(): boolean;
  estimateRgba(rgba: Uint8Array, width: number, height: number, capturedAtMs: number): Promise<MonocularDepthPriorFrame | null>;
  dispose(): void;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function samplePrior(frame: MonocularDepthPriorFrame, u: number, v: number): number | null {
  if (frame.width <= 0 || frame.height <= 0 || frame.relativeDepth.length < frame.width * frame.height) return null;
  const x = clamp(Math.round(clamp(u, 0, 1) * (frame.width - 1)), 0, frame.width - 1);
  const y = clamp(Math.round(clamp(v, 0, 1) * (frame.height - 1)), 0, frame.height - 1);
  const value = frame.relativeDepth[y * frame.width + x];
  return Number.isFinite(value) && value > 1e-6 ? value : null;
}

interface LogSample { x: number; y: number; weight: number; }

function weightedLine(samples: readonly LogSample[], robust?: readonly number[]): { a: number; b: number } | null {
  let w = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (let index = 0; index < samples.length; index += 1) {
    const item = samples[index];
    const weight = Math.max(0, item.weight * (robust?.[index] ?? 1));
    w += weight; sx += item.x * weight; sy += item.y * weight;
    sxx += item.x * item.x * weight; sxy += item.x * item.y * weight;
  }
  const determinant = w * sxx - sx * sx;
  if (w <= 1e-8 || Math.abs(determinant) <= 1e-8) return null;
  return {
    a: (w * sxy - sx * sy) / determinant,
    b: (sy * sxx - sx * sxy) / determinant,
  };
}

/**
 * Robust calibration for a relative monocular prior:
 * log(z_metric) ~= a*log(z_prior)+b.
 *
 * Both polarities are accepted. Some exported depth networks expose a quantity
 * proportional to depth, while others expose inverse-depth/disparity-like
 * values. Physical ARCore anchors decide the sign and scale; model convention
 * is never assumed. The result is surface guidance only and is never reused for
 * occupancy/FREE classification.
 */
export function calibrateMonocularDepthPrior(
  frame: MonocularDepthPriorFrame,
  anchors: readonly MetricDepthAnchor[],
): LogAffineDepthCalibration | null {
  const samples: LogSample[] = [];
  for (const anchor of anchors) {
    if (!Number.isFinite(anchor.depthMeters) || anchor.depthMeters <= 0.08) continue;
    const prior = samplePrior(frame, anchor.u, anchor.v);
    if (prior === null) continue;
    samples.push({ x: Math.log(prior), y: Math.log(anchor.depthMeters), weight: clamp(anchor.weight ?? 1, 0.05, 1) });
  }
  if (samples.length < 6) return null;

  let best: { a: number; b: number; inliers: number[]; error: number } | null = null;
  const maxTrials = Math.min(72, samples.length * 3);
  for (let trial = 0; trial < maxTrials; trial += 1) {
    const first = trial % samples.length;
    const second = (trial * 7 + Math.floor(samples.length / 2) + 1) % samples.length;
    if (first === second) continue;
    const dx = samples[second].x - samples[first].x;
    if (Math.abs(dx) < 1e-5) continue;
    const a = (samples[second].y - samples[first].y) / dx;
    const b = samples[first].y - a * samples[first].x;
    const absoluteSlope = Math.abs(a);
    if (!Number.isFinite(a) || !Number.isFinite(b) || absoluteSlope < 0.25 || absoluteSlope > 4) continue;
    const inliers: number[] = [];
    let error = 0;
    for (let index = 0; index < samples.length; index += 1) {
      const residual = Math.abs(samples[index].y - (a * samples[index].x + b));
      if (residual <= 0.13) { inliers.push(index); error += residual * samples[index].weight; }
    }
    if (best === null || inliers.length > best.inliers.length || (inliers.length === best.inliers.length && error < best.error)) {
      best = { a, b, inliers, error };
    }
  }
  if (best === null || best.inliers.length < Math.max(5, Math.ceil(samples.length * 0.45))) return null;

  const active = best.inliers.map((index) => samples[index]);
  let fit = weightedLine(active);
  if (fit === null || Math.abs(fit.a) < 0.20 || Math.abs(fit.a) > 4.5) return null;
  for (let iteration = 0; iteration < 4; iteration += 1) {
    const residuals = active.map((sample) => Math.abs(sample.y - (fit!.a * sample.x + fit!.b))).sort((a, b) => a - b);
    const median = residuals[Math.floor(residuals.length * 0.5)] ?? 0.02;
    const huber = Math.max(0.035, median * 2.2);
    const weights = active.map((sample) => {
      const residual = Math.abs(sample.y - (fit!.a * sample.x + fit!.b));
      return residual <= huber ? 1 : huber / Math.max(residual, 1e-9);
    });
    fit = weightedLine(active, weights) ?? fit;
  }
  if (Math.abs(fit.a) < 0.20 || Math.abs(fit.a) > 4.5) return null;
  let squared = 0;
  for (const sample of active) {
    const residual = sample.y - (fit.a * sample.x + fit.b);
    squared += residual * residual;
  }
  const rms = Math.sqrt(squared / active.length);
  const inlierRatio = active.length / samples.length;
  return {
    scale: fit.a,
    bias: fit.b,
    inliers: active.length,
    rmsLogError: rms,
    confidence: clamp(inlierRatio * 0.62 + clamp(1 - rms / 0.12, 0, 1) * 0.38, 0, 1),
  };
}

export function calibratedPriorDepthMeters(
  frame: MonocularDepthPriorFrame,
  calibration: LogAffineDepthCalibration,
  u: number,
  v: number,
): number | null {
  const prior = samplePrior(frame, u, v);
  if (prior === null) return null;
  const metric = Math.exp(calibration.scale * Math.log(prior) + calibration.bias);
  return Number.isFinite(metric) && metric > 0.08 && metric < 12 ? metric : null;
}

interface TransformersModuleLike {
  pipeline(task: string, model: string, options?: Record<string, unknown>): Promise<(input: unknown) => Promise<unknown>>;
}

function tensorFromOutput(output: unknown): { data: ArrayLike<number>; width: number; height: number } | null {
  if (output === null || typeof output !== 'object') return null;
  const record = output as Record<string, unknown>;
  const candidate = (record.predicted_depth ?? record.depth) as Record<string, unknown> | undefined;
  if (candidate === undefined || candidate === null || typeof candidate !== 'object') return null;
  const data = candidate.data as ArrayLike<number> | undefined;
  const dims = candidate.dims as ArrayLike<number> | undefined;
  if (data !== undefined && dims !== undefined && dims.length >= 2) {
    const height = Number(dims[dims.length - 2]);
    const width = Number(dims[dims.length - 1]);
    if (width > 0 && height > 0 && data.length >= width * height) return { data, width, height };
  }
  const width = Number(candidate.width);
  const height = Number(candidate.height);
  if (data !== undefined && width > 0 && height > 0 && data.length >= width * height) return { data, width, height };
  return null;
}

/**
 * Optional browser-only DA-V2 provider. It is lazy-loaded from a CDN so the
 * model/runtime never enters ESP32 flash. Failure is non-fatal; the physical
 * RGB+ARCore pipeline remains fully functional offline.
 */
export class DepthAnythingV2WebGpuProvider implements MonocularDepthProvider {
  readonly name = 'Depth Anything V2 Small (optional WebGPU prior)';
  status: MonocularDepthProviderStatus = 'disabled';
  error: string | null = null;
  private estimator: ((input: unknown) => Promise<unknown>) | null = null;
  private disposed = false;

  constructor(
    private readonly model = 'onnx-community/depth-anything-v2-small',
    private readonly moduleUrl = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1/+esm',
  ) {}

  isSupported(): boolean {
    return typeof navigator !== 'undefined' && 'gpu' in navigator && typeof document !== 'undefined';
  }

  private async load(): Promise<boolean> {
    if (this.estimator !== null) return true;
    if (this.disposed || !this.isSupported()) { this.status = 'unavailable'; return false; }
    this.status = 'loading';
    try {
      const dynamicImport = new Function('url', 'return import(url)') as (url: string) => Promise<TransformersModuleLike>;
      const module = await dynamicImport(this.moduleUrl);
      this.estimator = await module.pipeline('depth-estimation', this.model, { device: 'webgpu', dtype: 'q4f16' });
      this.status = 'ready';
      this.error = null;
      return true;
    } catch (error) {
      this.status = 'error';
      this.error = error instanceof Error ? error.message : String(error);
      return false;
    }
  }

  async estimateRgba(
    rgba: Uint8Array,
    width: number,
    height: number,
    capturedAtMs: number,
  ): Promise<MonocularDepthPriorFrame | null> {
    if (!(await this.load()) || this.estimator === null || this.disposed) return null;
    const canvas = document.createElement('canvas');
    canvas.width = width; canvas.height = height;
    const context = canvas.getContext('2d');
    const pixelCount = width * height * 4;
    if (context === null || rgba.length < pixelCount) return null;
    // TypeScript 7 tracks SharedArrayBuffer-capable views explicitly. ImageData
    // requires an owned ArrayBuffer-backed clamped array, so copy instead of
    // forwarding rgba.buffer (which may be typed as ArrayBufferLike).
    const pixels: Uint8ClampedArray<ArrayBuffer> = new Uint8ClampedArray(pixelCount);
    pixels.set(rgba.subarray(0, pixelCount));
    context.putImageData(new ImageData(pixels, width, height), 0, 0);
    try {
      const output = await this.estimator(canvas.toDataURL('image/jpeg', 0.82));
      const tensor = tensorFromOutput(output);
      if (tensor === null) throw new Error('Depth Anything output did not expose a depth tensor.');
      const relativeDepth = new Float32Array(tensor.width * tensor.height);
      for (let index = 0; index < relativeDepth.length; index += 1) relativeDepth[index] = Number(tensor.data[index]);
      this.status = 'ready'; this.error = null;
      return { width: tensor.width, height: tensor.height, relativeDepth, capturedAtMs, source: this.model };
    } catch (error) {
      this.status = 'error';
      this.error = error instanceof Error ? error.message : String(error);
      return null;
    }
  }

  dispose(): void { this.disposed = true; this.estimator = null; }
}
