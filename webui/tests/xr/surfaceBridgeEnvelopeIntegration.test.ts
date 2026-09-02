import { describe, expect, it } from 'vitest';

import { computeOperationalSurfaceEnvelope } from '../../src/xr/surfaceOperationalEnvelope';
import type { SurfaceComponent, SurfaceComponentAnalysis } from '../../src/xr/surfaceComponents';

function component(id: number, overrides: Partial<SurfaceComponent> = {}): SurfaceComponent {
  return {
    id,
    pointCount: 40,
    occupiedCellCount: 10,
    center: [0, 0, 0],
    majorExtentMeters: 0.22,
    minorExtentMeters: 0.12,
    thicknessMeters: 0.05,
    occupancyRatio: 0.65,
    meshReadiness: 0.9,
    meshEligible: false,
    ...overrides,
  };
}

function analysis(components: SurfaceComponent[], labels: number[]): SurfaceComponentAnalysis {
  return {
    pointCount: labels.length,
    acceptedPointCount: labels.filter((value) => value >= 0).length,
    componentCount: components.length,
    eligibleComponentCount: 0,
    eligiblePointCount: 0,
    eligiblePointMask: new Uint8Array(labels.length),
    componentIdByPoint: Int32Array.from(labels),
    components,
    largestComponent: components[0] ?? null,
    bestMeshComponent: components[0] ?? null,
    connectionCellSizeMeters: 0.06,
  };
}

function flatten(points: readonly (readonly [number, number, number])[]): Float32Array {
  return Float32Array.from(points.flatMap((point) => point));
}

describe('weak bridge evidence in UNKNOWN envelope policy', () => {
  it('requests rescan for a weakly bridged island without letting that island expand the envelope', () => {
    const primary = component(0, { meshEligible: true });
    const weakIsland = component(1, {
      pointCount: 4,
      occupiedCellCount: 2,
      center: [0.20, 0, 0],
      majorExtentMeters: 0.05,
      minorExtentMeters: 0.025,
      occupancyRatio: 0.12,
      meshReadiness: 0.20,
    });
    const positions = flatten([
      [0.00, 0, 0], [0.02, 0, 0],
      [0.06, 0, 0], [0.12, 0, 0],
      [0.18, 0, 0], [0.20, 0, 0],
    ]);
    const result = computeOperationalSurfaceEnvelope(
      positions,
      analysis([primary, weakIsland], [0, 0, -1, -1, 1, 1]),
      0.04,
      { min: [0, 0, 0], max: [0.20, 0, 0] },
    );

    expect(result.retainedComponentIds).toEqual([0]);
    expect(result.quarantinedComponentIds).toEqual([1]);
    expect(result.rescanBridgeComponentIds).toEqual([1]);
    expect(result.bounds?.max[0]).toBeCloseTo(0.02, 5);
    expect(result.quarantinedPointCount).toBe(2);
  });
});
