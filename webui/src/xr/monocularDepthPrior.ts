import {
  calibrateRelativeDepthToMetricDetailed,
  metricDepthFromRelative,
  type RelativeDepthCalibration,
  type RelativeDepthCalibrationDiagnostics,
  type RelativeMetricSample,
} from './monocularDepthCalibration';

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

/** Backward-compatible name used by runtime/tests; model is now explicitly tagged. */
export type LogAffineDepthCalibration = RelativeDepthCalibration;
export type MonocularDepthCalibrationDiagnostics = RelativeDepthCalibrationDiagnostics;

export type MonocularDepthProviderStatus = 'disabled' | 'unavailable' | 'loading' | 'ready' | 'error';

export interface MonocularDepthProvider {
  readonly name: string;
  readonly status: MonocularDepthProviderStatus;
  readonly error: string | null;
  isSupported(): boolean;
  estimateRgba(rgba: Uint8Array, width: number, height: number, capturedAtMs: number): Promise<MonocularDepthPriorFrame | null>;
  releaseMemory?(): Promise<void> | void;
  dispose(): Promise<void> | void;
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

function calibrationSamples(
  frame: MonocularDepthPriorFrame,
  anchors: readonly MetricDepthAnchor[],
): RelativeMetricSample[] {
  const samples: RelativeMetricSample[] = [];
  for (const anchor of anchors) {
    if (!Number.isFinite(anchor.depthMeters) || anchor.depthMeters <= 0.08) continue;
    const relativeDepth = samplePrior(frame, anchor.u, anchor.v);
    if (relativeDepth === null) continue;
    samples.push({
      relativeDepth,
      metricMeters: anchor.depthMeters,
      weight: clamp(anchor.weight ?? 1, 0.05, 1),
    });
  }
  return samples;
}

/**
 * Robust calibration for a relative monocular prior.
 *
 * Depth Anything V2 relative output does not promise a metric scale or a single
 * depth convention. We therefore test three physically anchored mappings:
 *   1) affine inverse depth/disparity: 1/z = a*r + b,
 *   2) affine depth:                 z   = a*r + b,
 *   3) legacy log-power fallback:    log(z) = a*log(r) + b.
 * The model with the strongest ARCore-anchor support and lowest metric log
 * residual wins. This remains a post-scan SURFACE prior only; it never creates
 * FREE/occupied evidence.
 */
export function calibrateMonocularDepthPriorDetailed(
  frame: MonocularDepthPriorFrame,
  anchors: readonly MetricDepthAnchor[],
): { calibration: LogAffineDepthCalibration | null; diagnostics: MonocularDepthCalibrationDiagnostics } {
  return calibrateRelativeDepthToMetricDetailed(calibrationSamples(frame, anchors));
}

export function calibrateMonocularDepthPrior(
  frame: MonocularDepthPriorFrame,
  anchors: readonly MetricDepthAnchor[],
): LogAffineDepthCalibration | null {
  return calibrateMonocularDepthPriorDetailed(frame, anchors).calibration;
}

export function calibratedPriorDepthMeters(
  frame: MonocularDepthPriorFrame,
  calibration: LogAffineDepthCalibration,
  u: number,
  v: number,
): number | null {
  const relativeDepth = samplePrior(frame, u, v);
  return relativeDepth === null ? null : metricDepthFromRelative(relativeDepth, calibration);
}

export interface TransformersRawImageConstructor {
  new (data: Uint8Array | Uint8ClampedArray, width: number, height: number, channels: 4): unknown;
}

export type TransformersDepthPipeline = ((input: unknown) => Promise<unknown>) & {
  dispose?: () => Promise<void> | void;
};

export interface TransformersModuleLike {
  pipeline(task: string, model: string, options?: Record<string, unknown>): Promise<TransformersDepthPipeline>;
  RawImage?: TransformersRawImageConstructor;
}

/** Create an exact, uncompressed Transformers.js image input from staged XR RGBA. */
export function createTransformersRawImageInput(
  rgba: Uint8Array,
  width: number,
  height: number,
  RawImage: TransformersRawImageConstructor,
): unknown | null {
  const pixelCount = width * height * 4;
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width <= 0 || height <= 0 || rgba.length < pixelCount) return null;
  // The staged RGB packet already owns its byte array, so no JPEG encode/decode
  // and no second full-frame copy are necessary here.
  return new RawImage(rgba.length === pixelCount ? rgba : rgba.subarray(0, pixelCount), width, height, 4);
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

function canvasModelInput(rgba: Uint8Array, width: number, height: number): string | null {
  if (typeof document === 'undefined') return null;
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext('2d');
  const pixelCount = width * height * 4;
  if (context === null || rgba.length < pixelCount) return null;
  // TypeScript 7 tracks SharedArrayBuffer-capable views explicitly. ImageData
  // requires an owned ArrayBuffer-backed clamped array.
  const pixels: Uint8ClampedArray<ArrayBuffer> = new Uint8ClampedArray(pixelCount);
  pixels.set(rgba.subarray(0, pixelCount));
  context.putImageData(new ImageData(pixels, width, height), 0, 0);
  return canvas.toDataURL('image/png');
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
  private estimator: TransformersDepthPipeline | null = null;
  private rawImageConstructor: TransformersRawImageConstructor | null = null;
  private generation = 0;
  private loadPromise: Promise<boolean> | null = null;
  private releasePromise: Promise<void> | null = null;
  private readonly releasedPipelines = new WeakSet<object>();

  constructor(
    private readonly model = 'onnx-community/depth-anything-v2-small',
    private readonly moduleUrl = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1/+esm',
    private readonly moduleLoader?: () => Promise<TransformersModuleLike>,
  ) {}

  isSupported(): boolean {
    return typeof navigator !== 'undefined' && 'gpu' in navigator && typeof document !== 'undefined';
  }

  private load(): Promise<boolean> {
    if (this.releasePromise !== null) return this.releasePromise.then(() => this.load());
    if (this.estimator !== null) return Promise.resolve(true);
    if (this.loadPromise !== null) return this.loadPromise;
    if (!this.isSupported()) { this.status = 'unavailable'; return Promise.resolve(false); }
    const generation = this.generation;
    this.status = 'loading';
    const pending = this.createEstimator(generation).finally(() => {
      if (this.loadPromise === pending) this.loadPromise = null;
    });
    this.loadPromise = pending;
    return pending;
  }

  private async createEstimator(generation: number): Promise<boolean> {
    try {
      const module = this.moduleLoader === undefined
        ? await (new Function('url', 'return import(url)') as (url: string) => Promise<TransformersModuleLike>)(this.moduleUrl)
        : await this.moduleLoader();
      if (generation !== this.generation) return false;
      const estimator = await module.pipeline('depth-estimation', this.model, { device: 'webgpu', dtype: 'q4f16' });
      if (generation !== this.generation) {
        await this.releasePipeline(estimator);
        return false;
      }
      this.estimator = estimator;
      this.rawImageConstructor = module.RawImage ?? null;
      this.status = 'ready';
      this.error = null;
      return true;
    } catch (error) {
      if (generation === this.generation) {
        this.status = 'error';
        this.error = error instanceof Error ? error.message : String(error);
      }
      return false;
    }
  }

  private async releasePipeline(pipeline: TransformersDepthPipeline): Promise<void> {
    if (this.releasedPipelines.has(pipeline)) return;
    this.releasedPipelines.add(pipeline);
    await pipeline.dispose?.();
  }

  async estimateRgba(
    rgba: Uint8Array,
    width: number,
    height: number,
    capturedAtMs: number,
  ): Promise<MonocularDepthPriorFrame | null> {
    if (!(await this.load())) return null;
    const generation = this.generation;
    const estimator = this.estimator;
    if (estimator === null) return null;
    try {
      const rawInput = this.rawImageConstructor === null
        ? null
        : createTransformersRawImageInput(rgba, width, height, this.rawImageConstructor);
      // RawImage is the normal fast path. Keep a lossless canvas fallback for a
      // module build that does not export it; never JPEG-compress structural RGB.
      const input = rawInput ?? canvasModelInput(rgba, width, height);
      if (input === null) throw new Error('Unable to prepare Depth Anything image input.');
      const output = await estimator(input);
      if (generation !== this.generation || estimator !== this.estimator) return null;
      const tensor = tensorFromOutput(output);
      if (tensor === null) throw new Error('Depth Anything output did not expose a depth tensor.');
      const relativeDepth = new Float32Array(tensor.width * tensor.height);
      for (let index = 0; index < relativeDepth.length; index += 1) relativeDepth[index] = Number(tensor.data[index]);
      this.status = 'ready';
      this.error = null;
      return { width: tensor.width, height: tensor.height, relativeDepth, capturedAtMs, source: this.model };
    } catch (error) {
      if (generation === this.generation && estimator === this.estimator) {
        this.status = 'error';
        this.error = error instanceof Error ? error.message : String(error);
      }
      return null;
    }
  }

  async releaseMemory(): Promise<void> {
    const precedingRelease = this.releasePromise;
    const loadPromise = this.loadPromise;
    this.generation += 1;
    this.loadPromise = null;
    const estimator = this.estimator;
    this.estimator = null;
    this.rawImageConstructor = null;
    this.status = 'disabled';
    this.error = null;
    const release = (async (): Promise<void> => {
      if (estimator !== null) await this.releasePipeline(estimator);
      if (loadPromise !== null) await loadPromise;
      if (precedingRelease !== null) await precedingRelease;
    })();
    this.releasePromise = release;
    try {
      await release;
    } finally {
      if (this.releasePromise === release) this.releasePromise = null;
    }
  }

  async dispose(): Promise<void> {
    await this.releaseMemory();
  }
}
