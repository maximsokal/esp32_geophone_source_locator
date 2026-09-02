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

function analysis(
  components: SurfaceComponent[],
  labels: number[],
): SurfaceComponentAnalysis {
  const largest = [...components].sort((a, b) => b.pointCount - a.pointCount || a.id - b.id)[0] ?? null;
  return {
    pointCount: labels.length,
    acceptedPointCount: labels.filter((value) => value >= 0).length,
    componentCount: components.length,
    eligibleComponentCount: components.filter((item) => item.meshEligible).length,
    eligiblePointCount: 0,
    eligiblePointMask: new Uint8Array(labels.length),
    componentIdByPoint: Int32Array.from(labels),
    components,
    largestComponent: largest,
    bestMeshComponent: components[0] ?? null,
    connectionCellSizeMeters: 0.06,
  };
}

function flatten(points: readonly (readonly [number, number, number])[]): Float32Array {
  return Float32Array.from(points.flatMap((point) => point));
}

describe('component-aware physical search envelope', () => {
  it('does not let a tiny distant secondary island inflate the UNKNOWN candidate volume', () => {
    const primary = component(0, { pointCount: 4 });
    const tinyFar = component(1, {
      pointCount: 3,
      occupiedCellCount: 1,
      center: [4, 0, 0],
      majorExtentMeters: 0.04,
      minorExtentMeters: 0.02,
      occupancyRatio: 0.12,
      meshReadiness: 0.18,
    });
    const positions = flatten([
      [0, 0, 0], [0.2, 0, 0], [0, 0.1, 0], [0.2, 0.1, 0],
      [4, 0, 0], [4.02, 0, 0], [4.01, 0.01, 0],
    ]);
    const result = computeOperationalSurfaceEnvelope(
      positions,
      analysis([primary, tinyFar], [0, 0, 0, 0, 1, 1, 1]),
      0.04,
      { min: [0, 0, 0], max: [4.02, 0.1, 0] },
    );
    expect(result.retainedComponentIds).toEqual([0]);
    expect(result.quarantinedComponentIds).toEqual([1]);
    expect(result.bounds!.max[0]).toBeCloseTo(0.2, 5);
    expect(result.quarantinedPointCount).toBe(3);
  });

  it('allows a substantial disconnected measured component to expand the search envelope', () => {
    const primary = component(0, { pointCount: 4 });
    const second = component(1, { pointCount: 24, meshReadiness: 0.75, center: [1.2, 0, 0] });
    const positions = flatten([
      [0, 0, 0], [0.2, 0, 0], [0, 0.1, 0], [0.2, 0.1, 0],
      [1.0, 0, 0], [1.2, 0, 0], [1.0, 0.1, 0], [1.2, 0.1, 0],
    ]);
    const result = computeOperationalSurfaceEnvelope(
      positions,
      analysis([primary, second], [0, 0, 0, 0, 1, 1, 1, 1]),
      0.04,
      { min: [0, 0, 0], max: [1.2, 0.1, 0] },
    );
    expect(result.retainedComponentIds).toEqual([0, 1]);
    expect(result.quarantinedComponentIds).toEqual([]);
    expect(result.bounds!.max[0]).toBeCloseTo(1.2, 5);
  });

  it('always keeps the primary component during an early tiny scan', () => {
    const primary = component(0, {
      pointCount: 3,
      occupiedCellCount: 1,
      majorExtentMeters: 0.03,
      minorExtentMeters: 0.02,
      occupancyRatio: 0.1,
      meshReadiness: 0.1,
    });
    const positions = flatten([[0, 0, 0], [0.03, 0, 0], [0, 0.02, 0]]);
    const result = computeOperationalSurfaceEnvelope(
      positions,
      analysis([primary], [0, 0, 0]),
      0.04,
      null,
    );
    expect(result.retainedComponentIds).toEqual([0]);
    expect(result.bounds).not.toBeNull();
  });

  it('falls back conservatively when component membership is unavailable', () => {
    const primary = component(0);
    const fixture = analysis([primary], [0, 0]);
    delete fixture.componentIdByPoint;
    const fallback = { min: [-1, -1, -1] as const, max: [1, 1, 1] as const };
    const result = computeOperationalSurfaceEnvelope(
      flatten([[0, 0, 0], [0.1, 0, 0]]),
      fixture,
      0.04,
      fallback,
    );
    expect(result.bounds).toEqual(fallback);
  });
});
