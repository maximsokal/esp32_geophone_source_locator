import { describe, expect, it } from 'vitest';

import type { CpuDepthInformationLike } from '../../src/xr/depthPointCloudV2';
import type {
  MonocularDepthPriorFrame,
  MonocularDepthProvider,
  MonocularDepthProviderStatus,
} from '../../src/xr/monocularDepthPrior';
import {
  collectPhysicalDepthGuard,
  MonocularPriorRuntime,
  physicalDepthGuardAllowsAiVeto,
} from '../../src/xr/monocularPriorRuntime';
import type { RgbDepthGuidanceFrame } from '../../src/xr/rgbDepthGuidance';

function relativeAt(u: number, v: number): number { return 0.55 + u * 0.72 + v * 0.24; }

function guidance(capturedAtMs: number): RgbDepthGuidanceFrame {
  const width = 64, height = 48;
  return {
    width,
    height,
    luma: new Uint8Array(width * height).fill(128),
    rgba: new Uint8Array(width * height * 4).fill(127),
    edgeStrength: new Uint8Array(width * height),
    gradientX: new Int8Array(width * height),
    gradientY: new Int8Array(width * height),
    projectionMatrix: new Float32Array([
      1,0,0,0,
      0,1,0,0,
      0,0,0,-1,
      0,0,0,0,
    ]),
    worldFromView: new Float32Array([
      1,0,0,0,
      0,1,0,0,
      0,0,1,0,
      0,0,0,1,
    ]),
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

class CountingProvider implements MonocularDepthProvider {
  readonly name = 'counting-onnx-provider';
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

describe('post-scan monocular prior scheduling', () => {
  it('stages RGB + physical anchors during live XR without executing the provider', async () => {
    const provider = new CountingProvider();
    const runtime = new MonocularPriorRuntime(provider);
    runtime.setEnabled(true);

    expect(runtime.stagePostScan(guidance(100), metricDepth())).toBe(true);
    expect(runtime.hasStagedPostScanFrame).toBe(true);
    expect(provider.calls).toBe(0);
    expect(runtime.isBusy).toBe(false);
    expect(runtime.calibrationDiagnostics).toBeNull();

    expect(await runtime.runStagedPostScan()).toBe(true);
    expect(provider.calls).toBe(1);
    expect(runtime.hasStagedPostScanFrame).toBe(false);
    expect(runtime.calibrationConfidence).toBeGreaterThan(0.65);
    expect(runtime.calibrationDiagnostics).not.toBeNull();
    expect(runtime.calibrationDiagnostics!.sampleCount).toBeGreaterThanOrEqual(10);
    expect(runtime.calibrationDiagnostics!.selectedModel).not.toBeNull();
  });

  it('stages only while the phone has headroom: an earlier frame survives a busy phone, no new one is staged, and the inference still runs after the scan', async () => {
    const provider = new CountingProvider();
    const runtime = new MonocularPriorRuntime(provider);
    runtime.setEnabled(true);
    expect(runtime.isStagingAllowed).toBe(true);
    expect(runtime.stagePostScan(guidance(100), metricDepth())).toBe(true);
    runtime.setStagingAllowed(false);
    expect(runtime.stagePostScan(guidance(200), metricDepth())).toBe(false);
    expect(runtime.hasStagedPostScanFrame).toBe(true);
    expect(provider.calls).toBe(0);
    runtime.setStagingAllowed(true);
    expect(runtime.stagePostScan(guidance(300), metricDepth())).toBe(true);
    runtime.setStagingAllowed(false);
    expect(await runtime.runStagedPostScan()).toBe(true); // gating applies to staging only, not to the post-scan run
    expect(provider.calls).toBe(1);
  });

  it('stages nothing, and runs nothing, when the phone never had headroom during the scan', async () => {
    const provider = new CountingProvider();
    const runtime = new MonocularPriorRuntime(provider);
    runtime.setEnabled(true);
    runtime.setStagingAllowed(false);
    for (let keyframe = 0; keyframe < 5; keyframe += 1) expect(runtime.stagePostScan(guidance(100 + keyframe), metricDepth())).toBe(false);
    expect(runtime.hasStagedPostScanFrame).toBe(false);
    expect(await runtime.runStagedPostScan()).toBe(false);
    expect(provider.calls).toBe(0);
  });

  it('guards AI veto with physical frontmost depth from the same staged RGB pose', () => {
    const depth = metricDepth();
    const frame = guidance(100);
    const guard = collectPhysicalDepthGuard(depth, frame);
    const u = 0.52, v = 0.47;
    const frontmost = depth.getDepthInMeters(u, v);

    expect(physicalDepthGuardAllowsAiVeto(guard, u, v, frontmost)).toBe(true);
    // A valid surface hidden well behind the staged foreground is not evidence
    // that AI may judge from this one RGB frame.
    expect(physicalDepthGuardAllowsAiVeto(guard, u, v, frontmost + 0.8)).toBe(false);
  });

  it('latest live keyframe can replace the staged packet repeatedly without provider work', () => {
    const provider = new CountingProvider();
    const runtime = new MonocularPriorRuntime(provider);
    runtime.setEnabled(true);

    for (let keyframe = 0; keyframe < 25; keyframe += 1) {
      expect(runtime.stagePostScan(guidance(100 + keyframe), metricDepth())).toBe(true);
    }
    expect(provider.calls).toBe(0);
    expect(runtime.hasStagedPostScanFrame).toBe(true);
    expect(runtime.calibrationDiagnostics).toBeNull();
  });

  it('consumes a staged packet exactly once so viewport remounts cannot rerun a multi-second model', async () => {
    const provider = new CountingProvider();
    const runtime = new MonocularPriorRuntime(provider);
    runtime.setEnabled(true);

    expect(runtime.stagePostScan(guidance(500), metricDepth())).toBe(true);
    expect(await runtime.runStagedPostScan()).toBe(true);
    expect(provider.calls).toBe(1);
    expect(runtime.hasStagedPostScanFrame).toBe(false);

    expect(await runtime.runStagedPostScan()).toBe(false);
    expect(provider.calls).toBe(1);
  });

  it('clears previous calibration diagnostics when a new scan epoch starts', async () => {
    const runtime = new MonocularPriorRuntime(new CountingProvider());
    runtime.setEnabled(true);
    expect(runtime.stagePostScan(guidance(100), metricDepth())).toBe(true);
    expect(await runtime.runStagedPostScan()).toBe(true);
    expect(runtime.calibrationDiagnostics).not.toBeNull();

    runtime.reset();
    expect(runtime.calibrationDiagnostics).toBeNull();
    expect(runtime.calibrationConfidence).toBe(0);
  });

  it('notifies UI subscribers when AI becomes armed, starts inference, and finishes calibration', async () => {
    const runtime = new MonocularPriorRuntime(new CountingProvider());
    let notifications = 0;
    const unsubscribe = runtime.subscribe(() => { notifications += 1; });
    expect(notifications).toBe(1);

    runtime.setEnabled(true);
    expect(notifications).toBe(2);
    expect(runtime.stagePostScan(guidance(100), metricDepth())).toBe(true);
    expect(notifications).toBe(3);

    expect(await runtime.runStagedPostScan()).toBe(true);
    expect(notifications).toBeGreaterThanOrEqual(5);
    const beforeUnsubscribe = notifications;
    unsubscribe();
    runtime.reset();
    expect(notifications).toBe(beforeUnsubscribe);
  });
});
