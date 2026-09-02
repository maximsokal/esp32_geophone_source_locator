import { describe, expect, it } from 'vitest';

import { prepareCpuDepthSampler } from '../../src/xr/cpuDepthSampler';
import type { CpuDepthInformationLike } from '../../src/xr/depthPointCloudV2';

const identity = new Float32Array([
  1,0,0,0,
  0,1,0,0,
  0,0,1,0,
  0,0,0,1,
]);

function sourceWithRaw(
  values: Uint16Array | Float32Array,
  width: number,
  height: number,
  scale: number,
  matrix = identity,
): CpuDepthInformationLike {
  let nativeCalls = 0;
  const source = {
    width,
    height,
    data: values.buffer,
    rawValueToMeters: scale,
    normDepthBufferFromNormView: { matrix },
    getDepthInMeters(): number {
      nativeCalls += 1;
      return -999;
    },
    get nativeCalls(): number { return nativeCalls; },
  };
  return source as unknown as CpuDepthInformationLike;
}

describe('zero-copy XRCPUDepthInformation sampler', () => {
  it('reads luminance-alpha as a row-major Uint16 depth buffer without native calls', () => {
    const source = sourceWithRaw(new Uint16Array([
      100, 200, 300, 400,
      500, 600, 700, 800,
    ]), 4, 2, 0.001) as CpuDepthInformationLike & { readonly nativeCalls: number };
    const prepared = prepareCpuDepthSampler(source, 'luminance-alpha');

    expect(prepared.directBuffer).toBe(true);
    expect(prepared.sampler.getDepthInMeters(0.10, 0.10)).toBeCloseTo(0.1, 6);
    expect(prepared.sampler.getDepthInMeters(0.60, 0.10)).toBeCloseTo(0.3, 6);
    expect(prepared.sampler.getDepthInMeters(0.60, 0.75)).toBeCloseTo(0.7, 6);
    expect(source.nativeCalls).toBe(0);
  });

  it('supports float32 depth buffers with rawValueToMeters scaling', () => {
    const source = sourceWithRaw(new Float32Array([1.25, 2.5, 3.75, 5]), 2, 2, 0.5);
    const prepared = prepareCpuDepthSampler(source, 'float32');
    expect(prepared.directBuffer).toBe(true);
    expect(prepared.sampler.getDepthInMeters(0.75, 0.75)).toBeCloseTo(2.5, 6);
  });

  it('preserves zero as WebXR invalid/no-depth instead of inventing a metric value', () => {
    const source = sourceWithRaw(new Uint16Array([0, 250]), 2, 1, 0.001);
    const sampler = prepareCpuDepthSampler(source, 'unsigned-short').sampler;
    expect(sampler.getDepthInMeters(0.1, 0.5)).toBe(0);
    expect(sampler.getDepthInMeters(0.9, 0.5)).toBeCloseTo(0.25, 6);
  });

  it('applies normDepthBufferFromNormView before depth-buffer indexing', () => {
    const translated = new Float32Array(identity);
    translated[12] = 0.5;
    const source = sourceWithRaw(new Uint16Array([100, 200, 300, 400]), 4, 1, 0.001, translated);
    const prepared = prepareCpuDepthSampler(source, 'unsigned-short');
    expect(prepared.directBuffer).toBe(true);
    // View x=0 maps to normalized depth x=0.5 -> column 2.
    expect(prepared.sampler.getDepthInMeters(0, 0.5)).toBeCloseTo(0.3, 6);
  });

  it('matches WebXR range semantics for normalized view coordinates', () => {
    const source = sourceWithRaw(new Uint16Array([100]), 1, 1, 0.001);
    const sampler = prepareCpuDepthSampler(source, 'luminance-alpha').sampler;
    expect(() => sampler.getDepthInMeters(-0.001, 0.5)).toThrow(RangeError);
    expect(() => sampler.getDepthInMeters(0.5, 1.001)).toThrow(RangeError);
  });

  it('fails open to the native sampler for an unknown format or missing raw fields', () => {
    let calls = 0;
    const source: CpuDepthInformationLike = {
      width: 2,
      height: 2,
      getDepthInMeters(): number { calls += 1; return 1.23; },
    };
    const prepared = prepareCpuDepthSampler(source, 'vendor-format');
    expect(prepared.directBuffer).toBe(false);
    expect(prepared.sampler).toBe(source);
    expect(prepared.sampler.getDepthInMeters(0.5, 0.5)).toBe(1.23);
    expect(calls).toBe(1);
  });
});
