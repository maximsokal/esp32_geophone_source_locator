import { describe, expect, it } from 'vitest';

import type { LiveCoverageSnapshot } from '../../src/xr/depthPointCloud';
import { composeCoverageRenderSnapshot } from '../../src/xr/webxrRuntime';

function coverageFixture(): LiveCoverageSnapshot {
  return {
    positions: new Float32Array([1, 2, 3, 4, 5, 6]),
    confidence: new Float32Array([0.2, 0.85]),
    pointCount: 2,
    active: true,
    visibleCoverageCells: 3,
    readyCoverageCells: 1,
    totalCoverageCells: 4,
    minimumVisibleFrameHits: 2,
    targetFrameHits: 3,
    averageCandidatesPerSampledFrame: 12,
    promotedPointCount: 1,
    averageCoverageConfidence: 0.55,
  };
}

describe('composeCoverageRenderSnapshot', () => {
  it('preserves measured quality and appends unmistakable manual marker sentinels', () => {
    const result = composeCoverageRenderSnapshot(
      coverageFixture(),
      new Float32Array([7, 8, 9, 10, 11, 12]),
    );

    expect(result.pointCount).toBe(4);
    expect(Array.from(result.positions)).toEqual([
      1, 2, 3,
      4, 5, 6,
      7, 8, 9,
      10, 11, 12,
    ]);
    expect(result.confidence[0]).toBeCloseTo(0.2, 6);
    expect(result.confidence[1]).toBeCloseTo(0.85, 6);
    expect(result.confidence[2]).toBe(2);
    expect(result.confidence[3]).toBe(2);
  });

  it('honours pointCount bounds instead of reading stale capacity data', () => {
    const coverage = coverageFixture();
    coverage.pointCount = 1;
    const result = composeCoverageRenderSnapshot(coverage, new Float32Array(0));
    expect(result.pointCount).toBe(1);
    expect(Array.from(result.positions)).toEqual([1, 2, 3]);
    expect(result.confidence[0]).toBeCloseTo(0.2, 6);
  });

  it('reuses growable composition buffers and explicitly reserves the manual tail', () => {
    const reusable = { positions: new Float32Array(0), confidence: new Float32Array(0) };
    const first = composeCoverageRenderSnapshot(coverageFixture(), new Float32Array([7, 8, 9]), reusable);
    const positionsBuffer = first.positions.buffer;const confidenceBuffer = first.confidence.buffer;
    const second = composeCoverageRenderSnapshot(coverageFixture(), new Float32Array([10, 11, 12]), reusable);
    expect(first.manualPointCount).toBe(1);expect(second.manualPointCount).toBe(1);
    expect(first.positions.buffer).toBe(positionsBuffer);expect(second.positions.buffer).toBe(positionsBuffer);
    expect(first.confidence.buffer).toBe(confidenceBuffer);expect(second.confidence.buffer).toBe(confidenceBuffer);
    expect(second.positions.length).toBe(9);expect(second.confidence.length).toBe(3);
    expect(Array.from(second.positions)).toEqual([1, 2, 3, 4, 5, 6, 10, 11, 12]);
  });
});
