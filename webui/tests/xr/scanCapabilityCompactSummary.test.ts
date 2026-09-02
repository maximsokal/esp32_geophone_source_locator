import { describe, expect, it } from 'vitest';

import {
  computeScanCapabilityReadiness,
  summarizeSurfaceComponentReadiness,
} from '../../src/xr/scanCapabilityReadiness';
import type { SurfaceComponent, SurfaceComponentAnalysis } from '../../src/xr/surfaceComponents';

function component(
  id: number,
  overrides: Partial<SurfaceComponent> = {},
): SurfaceComponent {
  return {
    id,
    pointCount: 80,
    occupiedCellCount: 12,
    center: [id * 0.4, 0, 0],
    majorExtentMeters: 0.24,
    minorExtentMeters: 0.14,
    thicknessMeters: 0.05,
    occupancyRatio: 0.65,
    meshReadiness: 1,
    meshEligible: true,
    ...overrides,
  };
}

function analysis(components: readonly SurfaceComponent[]): SurfaceComponentAnalysis {
  const bestMeshComponent = [...components].sort((a, b) => b.meshReadiness - a.meshReadiness)[0] ?? null;
  return {
    pointCount: components.reduce((sum, item) => sum + item.pointCount, 0),
    acceptedPointCount: components.reduce((sum, item) => sum + item.pointCount, 0),
    componentCount: components.length,
    eligibleComponentCount: components.filter((item) => item.meshEligible).length,
    eligiblePointCount: components.filter((item) => item.meshEligible).reduce((sum, item) => sum + item.pointCount, 0),
    eligiblePointMask: new Uint8Array(0),
    componentIdByPoint: new Int32Array(0),
    components,
    largestComponent: components[0] ?? null,
    bestMeshComponent,
    connectionCellSizeMeters: 0.06,
  };
}

const viewpoints = [
  { position: [-0.2, 0, 0.3] as const, keyframeId: 1 },
  { position: [0.2, 0, 0.3] as const, keyframeId: 2 },
  { position: [0, 0.2, 0.3] as const, keyframeId: 3 },
  { position: [0, -0.2, 0.3] as const, keyframeId: 4 },
  { position: [0.3, 0.2, 0.3] as const, keyframeId: 5 },
  { position: [-0.3, 0.2, 0.3] as const, keyframeId: 6 },
  { position: [0.3, -0.2, 0.3] as const, keyframeId: 7 },
  { position: [-0.3, -0.2, 0.3] as const, keyframeId: 8 },
];

describe('compact worker readiness summary', () => {
  it('returns only the two component records required by the live readiness UI', () => {
    const structural = component(1, { pointCount: 90, meshReadiness: 0.7, meshEligible: false });
    const mesh = component(2, { pointCount: 80, meshReadiness: 1, meshEligible: true });
    const full = analysis([structural, mesh]);

    const compact = summarizeSurfaceComponentReadiness(full);
    expect(Object.keys(compact).sort()).toEqual(['bestMeshComponent', 'structuralComponent']);
    expect(compact.structuralComponent?.id).toBe(1);
    expect(compact.bestMeshComponent?.id).toBe(2);
    expect('components' in compact).toBe(false);
    expect('eligiblePointMask' in compact).toBe(false);
  });

  it('produces the same readiness result from compact and legacy full component input', () => {
    const full = analysis([
      component(1, { pointCount: 90, meshReadiness: 0.76, meshEligible: false }),
      component(2, { pointCount: 80, meshReadiness: 1, meshEligible: true }),
    ]);
    const compact = summarizeSurfaceComponentReadiness(full);
    const shared = {
      stablePointCount: 220,
      keyframeCount: 10,
      visibleCoverageCells: 20,
      readyCoverageCells: 12,
      viewpoints,
    };

    expect(computeScanCapabilityReadiness({ ...shared, surfaceAnalysis: compact }))
      .toEqual(computeScanCapabilityReadiness({ ...shared, surfaceAnalysis: full }));
  });

  it('keeps S on the structural cloud while M comes from the independently certified mesh cloud', () => {
    const structuralComponent = component(11, {
      pointCount: 96,
      center: [0, 0, 0],
      majorExtentMeters: 0.31,
      minorExtentMeters: 0.12,
      meshReadiness: 0.45,
      meshEligible: false,
    });
    const structuralDistractor = component(12, {
      pointCount: 70,
      center: [0.6, 0, 0],
      majorExtentMeters: 0.18,
      minorExtentMeters: 0.08,
      meshReadiness: 1,
      meshEligible: true,
    });
    const meshComponent = component(22, {
      pointCount: 84,
      center: [0.1, 0, 0],
      majorExtentMeters: 0.28,
      minorExtentMeters: 0.13,
      meshReadiness: 1,
      meshEligible: true,
    });
    const meshDistractor = component(23, {
      pointCount: 120,
      center: [0.9, 0, 0],
      meshReadiness: 0.52,
      meshEligible: false,
    });

    const compact = summarizeSurfaceComponentReadiness(
      analysis([structuralComponent, structuralDistractor]),
      analysis([meshComponent, meshDistractor]),
    );
    expect(compact.structuralComponent?.id).toBe(11);
    expect(compact.bestMeshComponent?.id).toBe(22);

    const readiness = computeScanCapabilityReadiness({
      stablePointCount: 220,
      keyframeCount: 10,
      visibleCoverageCells: 20,
      readyCoverageCells: 12,
      viewpoints,
      surfaceAnalysis: compact,
    });
    expect(readiness.objectMajorExtentMeters).toBeCloseTo(0.31, 6);
    expect(readiness.objectMinorExtentMeters).toBeCloseTo(0.12, 6);
    expect(readiness.mesh?.ready).toBe(true);
    expect(readiness.mesh?.progress).toBe(1);
  });
});
