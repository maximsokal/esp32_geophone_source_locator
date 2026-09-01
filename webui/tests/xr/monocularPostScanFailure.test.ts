import { describe, expect, it } from 'vitest';

import type { CpuDepthInformationLike } from '../../src/xr/depthPointCloudV2';
import type {
  MonocularDepthPriorFrame,
  MonocularDepthProvider,
  MonocularDepthProviderStatus,
} from '../../src/xr/monocularDepthPrior';
import { MonocularPriorRuntime } from '../../src/xr/monocularPriorRuntime';
import type { RgbDepthGuidanceFrame } from '../../src/xr/rgbDepthGuidance';

class EmptyProvider implements MonocularDepthProvider {
  readonly name = 'empty-provider';
  readonly status: MonocularDepthProviderStatus = 'ready';
  readonly error: string | null = null;
  isSupported(): boolean { return true; }
  async estimateRgba(): Promise<MonocularDepthPriorFrame | null> { return null; }
  dispose(): void {}
}

function guidance(): RgbDepthGuidanceFrame {
  const width = 32, height = 24;
  return {
    width,
    height,
    luma: new Uint8Array(width * height).fill(120),
    rgba: new Uint8Array(width * height * 4).fill(120),
    edgeStrength: new Uint8Array(width * height),
    gradientX: new Int8Array(width * height),
    gradientY: new Int8Array(width * height),
    capturedAtMs: 100,
  };
}

function physicalDepth(): CpuDepthInformationLike {
  return {
    width: 160,
    height: 90,
    getDepthInMeters(u: number, v: number): number { return 1.1 + u * 0.4 + v * 0.2; },
  };
}

describe('post-scan AI failure diagnostics', () => {
  it('reports an empty provider result explicitly instead of leaving AI pending', async () => {
    const runtime = new MonocularPriorRuntime(new EmptyProvider());
    runtime.setEnabled(true);
    expect(runtime.stagePostScan(guidance(), physicalDepth())).toBe(true);
    expect(await runtime.runStagedPostScan()).toBe(false);
    expect(runtime.error).toContain('returned no depth frame');
    expect(runtime.calibrationConfidence).toBe(0);
  });
});
