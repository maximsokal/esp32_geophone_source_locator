import { describe, expect, it } from 'vitest';

import type { CpuDepthInformationLike } from '../../src/xr/depthPointCloudV2';
import type {
  MonocularDepthPriorFrame,
  MonocularDepthProvider,
  MonocularDepthProviderStatus,
} from '../../src/xr/monocularDepthPrior';
import { MonocularPriorRuntime } from '../../src/xr/monocularPriorRuntime';
import type { RgbDepthGuidanceFrame } from '../../src/xr/rgbDepthGuidance';

function relativeAt(u: number, v: number): number {
  return 0.55 + u * 0.72 + v * 0.24;
}

function metricAt(u: number, v: number): number {
  return Math.exp(1.15 * Math.log(relativeAt(u, v)) + Math.log(1.7));
}

function guidance(capturedAtMs = 100): RgbDepthGuidanceFrame {
  const width = 64;
  const height = 48;
  return {
    width,
    height,
    luma: new Uint8Array(width * height).fill(128),
    rgba: new Uint8Array(width * height * 4).fill(127),
    edgeStrength: new Uint8Array(width * height),
    gradientX: new Int8Array(width * height),
    gradientY: new Int8Array(width * height),
    projectionMatrix: new Float32Array([
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, -1.020202, -1,
      0, 0, -0.2020202, 0,
    ]),
    worldFromView: new Float32Array([
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
    ]),
    capturedAtMs,
  };
}

function metricDepth(): CpuDepthInformationLike {
  return {
    width: 160,
    height: 90,
    getDepthInMeters(u: number, v: number): number {
      return metricAt(u, v);
    },
  };
}

class LocalOutlierProvider implements MonocularDepthProvider {
  readonly name = 'local-outlier-provider';
  status: MonocularDepthProviderStatus = 'ready';
  error: string | null = null;
  isSupported(): boolean { return true; }
  async estimateRgba(
    _rgba: Uint8Array,
    width: number,
    height: number,
    capturedAtMs: number,
  ): Promise<MonocularDepthPriorFrame> {
    const relativeDepth = new Float32Array(width * height);
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        const u = x / (width - 1);
        const v = y / (height - 1);
        relativeDepth[y * width + x] = relativeAt(u, v);
      }
    }
    // Calibration anchors are on an 8x6 half-cell grid and do not land on this
    // exact centre pixel. Make only the centre prior an obvious outlier so the
    // physical frontmost guard allows a real AI veto for the centre surfel.
    const centreX = Math.round(0.5 * (width - 1));
    const centreY = Math.round(0.5 * (height - 1));
    relativeDepth[centreY * width + centreX] *= 4;
    return { width, height, relativeDepth, capturedAtMs, source: this.name };
  }
  dispose(): void {}
}

function projectPointAt(u: number, v: number, depthMeters: number): readonly [number, number, number] {
  // With the identity XR pose and fx=fy=1 used above:
  // ndcX = x/depth, ndcY = projectZ/depth.
  return [
    (u * 2 - 1) * depthMeters,
    depthMeters,
    (1 - v * 2) * depthMeters,
  ];
}

describe('allocation-light post-scan monocular surfel veto', () => {
  it('matches the diagnostic agreement path for both accepted and rejected surfels', async () => {
    const runtime = new MonocularPriorRuntime(new LocalOutlierProvider());
    runtime.setEnabled(true);
    expect(runtime.stagePostScan(guidance(), metricDepth())).toBe(true);
    expect(await runtime.runStagedPostScan()).toBe(true);

    const acceptedPoint = projectPointAt(0.25, 0.25, metricAt(0.25, 0.25));
    const acceptedAgreement = runtime.postScanSurfaceAgreement(acceptedPoint);
    expect(acceptedAgreement.available).toBe(true);
    expect(acceptedAgreement.accepted).toBe(true);
    expect(runtime.postScanAcceptsSurfaceXYZ(
      acceptedPoint[0], acceptedPoint[1], acceptedPoint[2],
    )).toBe(true);

    const rejectedPoint = projectPointAt(0.5, 0.5, metricAt(0.5, 0.5));
    const rejectedAgreement = runtime.postScanSurfaceAgreement(rejectedPoint);
    expect(rejectedAgreement.available).toBe(true);
    expect(rejectedAgreement.accepted).toBe(false);
    expect(runtime.postScanAcceptsSurfaceXYZ(
      rejectedPoint[0], rejectedPoint[1], rejectedPoint[2],
    )).toBe(false);
  });

  it('fails open without a calibrated staged frame', () => {
    const runtime = new MonocularPriorRuntime(new LocalOutlierProvider());
    expect(runtime.postScanAcceptsSurfaceXYZ(0, 1.5, 0)).toBe(true);
    runtime.setEnabled(true);
    expect(runtime.postScanAcceptsSurfaceXYZ(0, 1.5, 0)).toBe(true);
  });
});
