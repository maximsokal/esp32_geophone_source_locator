import { describe, expect, it } from 'vitest';

import {
  computeScanCapabilityReadiness,
  ScanViewpointHistory,
} from '../../src/xr/scanCapabilityReadiness';
import type { SurfaceComponentAnalysis } from '../../src/xr/surfaceComponents';

function analysis(overrides: Partial<SurfaceComponentAnalysis['components'][number]> = {}): SurfaceComponentAnalysis {
  const component = {
    id: 0,
    pointCount: 80,
    occupiedCellCount: 18,
    center: [0, 0, 0] as const,
    majorExtentMeters: 0.32,
    minorExtentMeters: 0.20,
    thicknessMeters: 0.05,
    occupancyRatio: 0.72,
    meshReadiness: 1,
    meshEligible: true,
    ...overrides,
  };
  return {
    pointCount: component.pointCount,
    acceptedPointCount: component.pointCount,
    componentCount: 1,
    eligibleComponentCount: component.meshEligible ? 1 : 0,
    eligiblePointCount: component.meshEligible ? component.pointCount : 0,
    eligiblePointMask: new Uint8Array(component.pointCount).fill(component.meshEligible ? 1 : 0),
    components: [component],
    largestComponent: component,
    bestMeshComponent: component,
    connectionCellSizeMeters: 0.06,
  };
}

function view(keyframeId: number, x: number, y: number) {
  return { keyframeId, position: [x, y, 0.2] as const };
}

describe('scan capability readiness', () => {
  it('does not average away a missing prerequisite', () => {
    const readiness = computeScanCapabilityReadiness({
      stablePointCount: 900,
      keyframeCount: 2,
      visibleCoverageCells: 30,
      readyCoverageCells: 30,
      surfaceAnalysis: analysis(),
      viewpoints: [view(1, 0.5, 0), view(2, 0.55, 0)],
    });
    expect(readiness.structural.progress).toBeCloseTo(0.25, 6);
    expect(readiness.structural.limitingMetric).toBe('views');
    expect(readiness.structural.ready).toBe(false);
  });

  it('keeps structural analysis blocked when many points occupy a physically tiny component', () => {
    const readiness = computeScanCapabilityReadiness({
      stablePointCount: 500,
      keyframeCount: 20,
      visibleCoverageCells: 20,
      readyCoverageCells: 18,
      surfaceAnalysis: analysis({
        majorExtentMeters: 0.06,
        minorExtentMeters: 0.025,
        meshReadiness: 0.33,
        meshEligible: false,
      }),
      viewpoints: [view(1, 0.5, 0), view(2, -0.5, 0)],
    });
    expect(readiness.structural.progress).toBeLessThan(1);
    expect(readiness.structural.limitingMetric).toBe('extent');
    expect(readiness.objectTooSmall).toBe(true);
    expect(readiness.mesh?.ready).toBe(false);
  });

  it('reports mesh readiness independently from structural readiness', () => {
    const readiness = computeScanCapabilityReadiness({
      stablePointCount: 240,
      keyframeCount: 10,
      visibleCoverageCells: 12,
      readyCoverageCells: 9,
      surfaceAnalysis: analysis({
        majorExtentMeters: 0.16,
        minorExtentMeters: 0.06,
        meshReadiness: 0.80,
        meshEligible: false,
      }),
      viewpoints: [view(1, 0.4, 0), view(2, -0.4, 0)],
    });
    expect(readiness.structural.ready).toBe(true);
    expect(readiness.mesh?.progress).toBeCloseTo(0.80, 6);
    expect(readiness.mesh?.ready).toBe(false);
  });

  it('uses actual occupied azimuth sectors once a measured component centre exists', () => {
    const readiness = computeScanCapabilityReadiness({
      stablePointCount: 240,
      keyframeCount: 12,
      visibleCoverageCells: 12,
      readyCoverageCells: 9,
      surfaceAnalysis: analysis(),
      viewpoints: [
        view(1, 0.5, 0),
        view(2, 0, 0.5),
        view(3, -0.5, 0),
        view(4, 0, -0.5),
      ],
    });
    expect(readiness.viewSectors).toBe(4);
    expect(readiness.totalViewSectors).toBe(16);
    expect(readiness.viewCoverage).toBeCloseTo(0.25, 6);
  });
});

describe('bounded viewpoint history', () => {
  it('records each physical keyframe once and keeps bounded history', () => {
    const history = new ScanViewpointHistory(3);
    expect(history.record(view(1, 0, 0))).toBe(true);
    expect(history.record(view(1, 0.2, 0))).toBe(false);
    history.record(view(2, 0.1, 0));
    history.record(view(3, 0.2, 0));
    history.record(view(4, 0.3, 0));
    const snapshot = history.snapshot();
    expect(snapshot.map((item) => item.keyframeId)).toEqual([2, 3, 4]);
  });
});
