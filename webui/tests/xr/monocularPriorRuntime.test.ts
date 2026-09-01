import { describe, expect, it } from 'vitest';

import type { CpuDepthInformationLike } from '../../src/xr/depthPointCloudV2';
import type {
  MonocularDepthPriorFrame,
  MonocularDepthProvider,
  MonocularDepthProviderStatus,
} from '../../src/xr/monocularDepthPrior';
import {
  collectPhysicalMetricAnchors,
  MonocularPriorRuntime,
} from '../../src/xr/monocularPriorRuntime';
import type { RgbDepthGuidanceFrame } from '../../src/xr/rgbDepthGuidance';

function relativeAt(u: number, v: number): number {
  return 0.55 + u * 0.72 + v * 0.24;
}

function guidance(capturedAtMs: number): RgbDepthGuidanceFrame {
  const width = 64, height = 48;
  return {
    width,
    height,
    luma: new Uint8Array(width * height).fill(128),
    edgeStrength: new Uint8Array(width * height),
    gradientX: new Int8Array(width * height),
    gradientY: new Int8Array(width * height),
    capturedAtMs,
  };
}

function metricDepth(a = 1.15, b = Math.log(1.7)): CpuDepthInformationLike {
  return {
    width: 160,
    height: 90,
    getDepthInMeters(u: number, v: number): number {
      return Math.exp(a * Math.log(relativeAt(u, v)) + b);
    },
  };
}

class FakePriorProvider implements MonocularDepthProvider {
  readonly name = 'fake-prior';
  status: MonocularDepthProviderStatus = 'ready';
  error: string | null = null;
  calls = 0;

  isSupported(): boolean { return true; }

  async estimateRgba(
    _rgba: Uint8Array,
    width: number,
    height: number,
    capturedAtMs: number,
  ): Promise<MonocularDepthPriorFrame> {
    this.calls += 1;
    const relativeDepth = new Float32Array(width * height);
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        relativeDepth[y * width + x] = relativeAt(x / (width - 1), y / (height - 1));
      }
    }
    return { width, height, relativeDepth, capturedAtMs, source: this.name };
  }

  dispose(): void {}
}

describe('optional monocular prior runtime', () => {
  it('collects calibration anchors only from physical depth samples', () => {
    const anchors = collectPhysicalMetricAnchors(metricDepth(), guidance(100));
    expect(anchors.length).toBeGreaterThanOrEqual(40);
    expect(anchors.every((anchor) => Number.isFinite(anchor.depthMeters) && anchor.depthMeters > 0.18)).toBe(true);
  });

  it('does nothing until explicitly enabled', async () => {
    const provider = new FakePriorProvider();
    const runtime = new MonocularPriorRuntime(provider);
    expect(await runtime.maybeUpdate(guidance(100), metricDepth(), 100)).toBe(false);
    expect(provider.calls).toBe(0);
    expect(runtime.status).toBe('disabled');
  });

  it('calibrates against ARCore anchors and accepts an agreeing physical surface', async () => {
    const provider = new FakePriorProvider();
    const runtime = new MonocularPriorRuntime(provider);
    runtime.setEnabled(true);
    const frame = guidance(100);
    expect(await runtime.maybeUpdate(frame, metricDepth(), 100)).toBe(true);
    expect(provider.calls).toBe(1);
    expect(runtime.calibrationConfidence).toBeGreaterThan(0.65);

    const u = 0.55, v = 0.35;
    const physical = metricDepth().getDepthInMeters(u, v);
    const agreement = runtime.surfaceAgreement(u, v, physical, 200);
    expect(agreement.available).toBe(true);
    expect(agreement.accepted).toBe(true);
    expect(agreement.disagreementMeters!).toBeLessThan(0.1);
  });

  it('can only veto an extreme disagreement; it never substitutes its own metric depth', async () => {
    const runtime = new MonocularPriorRuntime(new FakePriorProvider());
    runtime.setEnabled(true);
    expect(await runtime.maybeUpdate(guidance(100), metricDepth(), 100)).toBe(true);

    const agreement = runtime.surfaceAgreement(0.5, 0.5, 6.5, 200);
    expect(agreement.available).toBe(true);
    expect(agreement.accepted).toBe(false);
    expect(agreement.priorMeters).not.toBeNull();
    // The API returns both measurements plus a veto decision; there is no
    // replacement-depth field that could manufacture geometry.
    expect(agreement.physicalMeters).toBe(6.5);
  });

  it('forgets calibrated model evidence immediately when disabled', async () => {
    const runtime = new MonocularPriorRuntime(new FakePriorProvider());
    runtime.setEnabled(true);
    expect(await runtime.maybeUpdate(guidance(100), metricDepth(), 100)).toBe(true);
    runtime.setEnabled(false);
    const agreement = runtime.surfaceAgreement(0.5, 0.5, 6.5, 200);
    expect(agreement.available).toBe(false);
    expect(agreement.accepted).toBe(true);
  });
});
