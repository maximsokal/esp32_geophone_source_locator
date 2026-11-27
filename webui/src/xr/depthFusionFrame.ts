import type { CpuDepthInformationLike } from './depthPointCloudV2';
import { prepareCpuDepthSampler, type WebXrDepthDataFormat } from './cpuDepthSampler';

/**
 * A depth frame in a form that can leave the XR animation frame (Phase 2, depth fusion worker).
 *
 * XRCPUDepthInformation and its buffer are valid only inside the frame callback and cannot be transferred, so the main thread takes one COPY of the raw
 * buffer (a few tens of kilobytes at ARCore's resolution) together with the numbers that define getDepthInMeters(): the worker then rebuilds the same
 * sampler with the same code (`prepareCpuDepthSampler`) and every depth read is bit-identical to the one the synchronous path would have made. When the
 * raw buffer is not available (old user agents, test doubles) the grid is sampled once at the pixel centres instead, which is equivalent for the
 * nearest-pixel reads the fusion makes.
 */
export interface DepthFrameBuffer {
  width: number;
  height: number;
  /** Row-major raw values: Uint16 (luminance-alpha / unsigned-short) or Float32 entries, `byteLength === width * height * entrySize`. */
  data: ArrayBuffer;
  format: 'uint16' | 'float32';
  rawValueToMeters: number;
  /** normDepthBufferFromNormView as a column-major 4x4. */
  normDepthBufferFromNormView: Float32Array;
  /** True when the buffer is the device's raw buffer, false when it is a grid sampled through getDepthInMeters(). */
  raw: boolean;
}

export interface DepthFrameGeometry {
  projectionMatrix: Float32Array;
  /** Column-major world-from-view of the depth view, WebXR frame (Y up). */
  worldFromView: Float32Array;
}

const IDENTITY = Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

/** Pool of equally sized ArrayBuffers: a transferred buffer comes back with the worker's answer, so steady state allocates nothing. */
export class DepthBufferPool {
  private readonly free = new Map<number, ArrayBuffer[]>();
  private outstanding = 0;

  constructor(readonly capacity: number) {}

  /** @returns null when `capacity` buffers are already out (the caller drops an optional frame instead of growing memory) */
  acquire(byteLength: number): ArrayBuffer | null {
    if (this.outstanding >= this.capacity) return null;
    const list = this.free.get(byteLength);
    const buffer = list?.pop() ?? new ArrayBuffer(byteLength);
    this.outstanding += 1;
    return buffer;
  }

  /** Gives a buffer back; a detached (transferred, not returned) buffer only lowers the outstanding count. */
  release(buffer: ArrayBuffer | null, byteLength: number): void {
    this.outstanding = Math.max(0, this.outstanding - 1);
    if (buffer === null || buffer.byteLength !== byteLength) return;
    const list = this.free.get(byteLength) ?? [];
    if (list.length < this.capacity) list.push(buffer);
    this.free.set(byteLength, list);
  }

  get inUse(): number { return this.outstanding; }
  get idle(): number { let n = 0; for (const list of this.free.values()) n += list.length; return n; }
  clear(): void { this.free.clear(); this.outstanding = 0; }
}

export function captureDepthFrameBuffer(depth: CpuDepthInformationLike, dataFormat: WebXrDepthDataFormat = null, pool?: DepthBufferPool): DepthFrameBuffer | null {
  const width = Math.floor(Number(depth.width)); const height = Math.floor(Number(depth.height));
  if (!(width > 0 && height > 0)) return null;
  const prepared = prepareCpuDepthSampler(depth, dataFormat);
  const source = depth as CpuDepthInformationLike & { data?: ArrayBuffer; rawValueToMeters?: number; normDepthBufferFromNormView?: { matrix: ArrayLike<number> } };
  if (prepared.directBuffer && prepared.inferredFormat !== null && source.data instanceof ArrayBuffer && source.normDepthBufferFromNormView !== undefined) {
    const entry = prepared.inferredFormat === 'uint16' ? 2 : 4;
    const byteLength = width * height * entry;
    const target = pool === undefined ? new ArrayBuffer(byteLength) : pool.acquire(byteLength);
    if (target === null) return null;
    new Uint8Array(target).set(new Uint8Array(source.data, 0, byteLength));
    return { width, height, data: target, format: prepared.inferredFormat, rawValueToMeters: Number(source.rawValueToMeters), normDepthBufferFromNormView: Float32Array.from(source.normDepthBufferFromNormView.matrix as ArrayLike<number>), raw: true };
  }
  const byteLength = width * height * 4;
  const target = pool === undefined ? new ArrayBuffer(byteLength) : pool.acquire(byteLength);
  if (target === null) return null;
  const grid = new Float32Array(target);
  for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
    let value = Number.NaN;
    try { value = depth.getDepthInMeters((x + 0.5) / width, (y + 0.5) / height); } catch { /* an unreadable pixel is a missing one */ }
    grid[y * width + x] = value;
  }
  return { width, height, data: target, format: 'float32', rawValueToMeters: 1, normDepthBufferFromNormView: IDENTITY.slice(), raw: false };
}

/** The sampler the fusion reads, rebuilt from a transferred frame with the code the synchronous path uses. */
export function samplerFromDepthFrameBuffer(frame: DepthFrameBuffer, geometry: DepthFrameGeometry): CpuDepthInformationLike {
  const like = {
    width: frame.width, height: frame.height, data: frame.data, rawValueToMeters: frame.rawValueToMeters,
    normDepthBufferFromNormView: { matrix: frame.normDepthBufferFromNormView },
    projectionMatrix: geometry.projectionMatrix, transform: { matrix: geometry.worldFromView },
    getDepthInMeters: (): number => Number.NaN,
  } as unknown as CpuDepthInformationLike;
  const prepared = prepareCpuDepthSampler(like, frame.format === 'uint16' ? 'unsigned-short' : 'float32');
  if (!prepared.directBuffer) throw new Error('depth frame buffer could not be turned into a sampler');
  return prepared.sampler;
}
