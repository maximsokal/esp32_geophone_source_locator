import type { CpuDepthInformationLike } from './depthPointCloudV2';

export type WebXrDepthDataFormat = 'luminance-alpha' | 'float32' | 'unsigned-short' | string | null;

interface CpuDepthRawBufferLike extends CpuDepthInformationLike {
  data?: ArrayBuffer;
  rawValueToMeters?: number;
  normDepthBufferFromNormView?: { matrix: ArrayLike<number> };
}

export interface PreparedCpuDepthSampler {
  sampler: CpuDepthInformationLike;
  directBuffer: boolean;
}

function finiteMatrix16(matrix: ArrayLike<number> | undefined): matrix is ArrayLike<number> {
  if (matrix === undefined || matrix.length < 16) return false;
  for (let index = 0; index < 16; index += 1) {
    if (!Number.isFinite(Number(matrix[index]))) return false;
  }
  return true;
}

function rawView(
  data: ArrayBuffer,
  format: WebXrDepthDataFormat,
  minimumEntries: number,
): Uint16Array | Float32Array | null {
  try {
    if (format === 'luminance-alpha' || format === 'unsigned-short') {
      const view = new Uint16Array(data);
      return view.length >= minimumEntries ? view : null;
    }
    if (format === 'float32') {
      const view = new Float32Array(data);
      return view.length >= minimumEntries ? view : null;
    }
  } catch {
    return null;
  }
  return null;
}

/**
 * Prepare a synchronous, allocation-free sampler for one active WebXR frame.
 *
 * XRCPUDepthInformation.data is row-major and getDepthInMeters() is specified as
 * exactly: normalized-view -> normDepthBufferFromNormView -> depth pixel -> raw
 * value * rawValueToMeters. Repeating that browser call for every surfel probe is
 * unnecessary overhead, so we perform the same algorithm in JS over the exposed
 * ArrayBuffer. The buffer is never mutated or retained beyond the frame call.
 *
 * If any required field is absent (older UA / test double / unknown format), the
 * original browser sampler is returned unchanged.
 */
export function prepareCpuDepthSampler(
  source: CpuDepthInformationLike,
  dataFormat: WebXrDepthDataFormat,
): PreparedCpuDepthSampler {
  const raw = source as CpuDepthRawBufferLike;
  const width = Math.floor(Number(source.width));
  const height = Math.floor(Number(source.height));
  const scale = Number(raw.rawValueToMeters);
  const transform = raw.normDepthBufferFromNormView?.matrix;
  if (
    !(raw.data instanceof ArrayBuffer) ||
    width <= 0 || height <= 0 ||
    !Number.isFinite(scale) || scale <= 0 ||
    !finiteMatrix16(transform)
  ) {
    return { sampler: source, directBuffer: false };
  }

  const values = rawView(raw.data, dataFormat, width * height);
  if (values === null) return { sampler: source, directBuffer: false };

  const m0 = Number(transform[0]);
  const m1 = Number(transform[1]);
  const m4 = Number(transform[4]);
  const m5 = Number(transform[5]);
  const m12 = Number(transform[12]);
  const m13 = Number(transform[13]);

  const sampler: CpuDepthInformationLike = {
    width,
    height,
    projectionMatrix: source.projectionMatrix,
    transform: source.transform,
    getDepthInMeters(x: number, y: number): number {
      if (!Number.isFinite(x) || !Number.isFinite(y) || x < 0 || x > 1 || y < 0 || y > 1) {
        throw new RangeError('WebXR normalized depth coordinates must be inside [0, 1].');
      }
      // XRRigidTransform is affine, so z=0,w=1 leaves these two rows only.
      const nx = m0 * x + m4 * y + m12;
      const ny = m1 * x + m5 * y + m13;
      if (!Number.isFinite(nx) || !Number.isFinite(ny)) return Number.NaN;
      const column = Math.max(0, Math.min(width - 1, Math.trunc(nx * width)));
      const row = Math.max(0, Math.min(height - 1, Math.trunc(ny * height)));
      return Number(values[row * width + column]) * scale;
    },
  };
  return { sampler, directBuffer: true };
}
