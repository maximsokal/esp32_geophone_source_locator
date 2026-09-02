import { describe, expect, it } from 'vitest';

import {
  DepthPointCloudAccumulator,
  MAX_EDGE_SUPERSAMPLE_PROBES_PER_FRAME,
  MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
} from '../../src/xr/depthPointCloudV2';

const identity = new Float32Array([
  1,0,0,0,
  0,1,0,0,
  0,0,1,0,
  0,0,0,1,
]);
const view: DepthSampleView = { projectionMatrix: identity, transform: { matrix: identity } };
const frame: DepthFusionFrameContext = {
  keyframeId: 1,
  evidenceKeyframe: true,
  frameWeight: 1,
  cameraProjectPosition: [0, 0, 0],
};

function depth(sample: (u: number, v: number) => number): CpuDepthInformationLike {
  return { width: 160, height: 90, getDepthInMeters: sample };
}

describe('adaptive physical depth probe budget', () => {
  it('keeps a flat scene on the base sampler with no extra probes', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const result = accumulator.sampleDepthFrame(depth(() => 1.5), view, frame);
    expect(result.edgeSupersampleProbes).toBe(0);
    expect(result.featureSupersampleProbes).toBe(0);
  });

  it('hard-caps discontinuity supersampling even in a stripe-heavy pathological frame', () => {
    const accumulator = new DepthPointCloudAccumulator(0.02, 5000, 5);
    // Vertical depth stripes: up/down remain supporting neighbours while many
    // left/right probes cross a >7.5 cm discontinuity, making edge expansion
    // frequent enough to saturate the frame budget.
    const striped = depth((u) => Math.floor(u * 80) % 2 === 0 ? 1.0 : 1.14);
    const result = accumulator.sampleDepthFrame(striped, view, frame);

    expect(result.edgeSupersampleProbes).toBeGreaterThan(0);
    expect(result.edgeSupersampleProbes).toBeLessThanOrEqual(MAX_EDGE_SUPERSAMPLE_PROBES_PER_FRAME);
    expect(result.edgeSupersampleProbes % 2).toBe(0);
    expect(result.featureSupersampleProbes).toBeLessThanOrEqual(MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME);
    expect(result.edgeSupersampleProbes + result.featureSupersampleProbes)
      .toBeLessThanOrEqual(MAX_EDGE_SUPERSAMPLE_PROBES_PER_FRAME + MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME);
  });
});
