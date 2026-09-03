import { describe, expect, it } from 'vitest';

import {
  DepthPointCloudAccumulator,
  MAX_FEATURE_SUPERSAMPLE_PROBES_PER_CELL_PER_FRAME,
  MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
} from '../../src/xr/depthPointCloudV2';

const projection = new Float32Array([
  12, 0, 0, 0,
  0, 12, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);

// Narrow FOV + translation keeps the sampled patch inside one 30 cm coverage
// cell. This makes the public retained-point count directly exercise the local
// allocation policy rather than differences in how many cells were visited.
const worldFromView = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0.15, 0.15, 0, 1,
]);
const view: DepthSampleView = {
  projectionMatrix: projection,
  transform: { matrix: worldFromView },
};

function depthSurface(sample: (u: number, v: number) => number): CpuDepthInformationLike {
  return { width: 320, height: 180, getDepthInMeters: sample };
}

const frame: DepthFusionFrameContext = {
  keyframeId: 1,
  evidenceKeyframe: true,
  frameWeight: 1,
  cameraProjectPosition: [0.15, 0, 0.15],
};

describe('feature-aware upstream surfel allocation', () => {
  it('actively re-probes tentative convex detail without granting single-view retention quota', () => {
    const flat = new DepthPointCloudAccumulator(0.01, 1000, 5);
    const convex = new DepthPointCloudAccumulator(0.01, 1000, 5);
    const flatDepth = depthSurface(() => 1.44);
    const convexDepth = depthSurface((u) => {
      const normalized = (u - 0.5) / 0.5;
      return 1.40 + 0.07 * normalized * normalized;
    });

    flat.sampleDepthFrame(flatDepth, view, frame);
    convex.sampleDepthFrame(convexDepth, view, frame);
    // The second sampled frame starts with a physically measured normal field in
    // the cell, allowing bounded feature-directed probes without any AI/native-plane trigger.
    const flatResult = flat.sampleDepthFrame(flatDepth, view, { ...frame, keyframeId: 2 });
    const convexResult = convex.sampleDepthFrame(convexDepth, view, { ...frame, keyframeId: 2 });

    expect(flat.coverageCount).toBe(1);
    expect(convex.coverageCount).toBe(1);
    expect(flat.pointsPerCoverageCell).toBe(28);
    expect(flat.pointCount).toBeLessThanOrEqual(30);
    expect(convex.pointCount).toBeLessThanOrEqual(28);
    expect(convex.pointCount).toBeLessThanOrEqual(convex.memoryBudget);
    expect(flatResult.featureSupersampleProbes).toBe(0);
    expect(convexResult.featureSupersampleProbes).toBeGreaterThan(0);
    expect(convexResult.featureSupersampleProbes).toBeLessThanOrEqual(MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME);
    // This fixture is intentionally confined to one coverage cell: it must not
    // consume the entire frame-wide feature budget even when that cell is curved.
    expect(convexResult.featureSupersampleProbes).toBeLessThanOrEqual(
      MAX_FEATURE_SUPERSAMPLE_PROBES_PER_CELL_PER_FRAME,
    );
    expect(convexResult.featureSupersampleProbes % 2).toBe(0);
  });
});
