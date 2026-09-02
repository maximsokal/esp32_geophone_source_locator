import { describe, expect, it } from 'vitest';

import {
  analyzeSensorSurfaceRelevance,
  type SensorSurfaceContext,
} from '../../src/xr/sensorAwareSurfaceRelevance';
import type { SurfaceComponent, SurfaceComponentAnalysis } from '../../src/xr/surfaceComponents';

function component(id: number, center: readonly [number, number, number]): SurfaceComponent {
  return {
    id,
    pointCount: 4,
    occupiedCellCount: 2,
    center,
    majorExtentMeters: 0.30,
    minorExtentMeters: 0.10,
    thicknessMeters: 0.05,
    occupancyRatio: 0.6,
    meshReadiness: 0.4,
    meshEligible: false,
  };
}

function analysis(components: SurfaceComponent[], labels: number[]): SurfaceComponentAnalysis {
  return {
    pointCount: labels.length,
    acceptedPointCount: labels.length,
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

function context(
  sensorPosition: readonly [number, number, number],
  overrides: Partial<SensorSurfaceContext> = {},
): SensorSurfaceContext {
  return {
    scanFrameId: 'XR_SCAN_17',
    sensorFrameId: 'XR_SCAN_17',
    sensors: [{ id: 'G1', position: sensorPosition, locked: true }],
    ...overrides,
  };
}

describe('frame-safe sensor relevance for surface islands', () => {
  it('is a strict no-op without a calibrated sensor context', () => {
    const result = analyzeSensorSurfaceRelevance(
      Float32Array.from([0, 0, 0]),
      analysis([component(0, [0, 0, 0])], [0]),
      null,
      0.04,
    );
    expect(result.status).toBe('unavailable');
    expect(result.relevantComponentIds).toEqual([]);
  });

  it('refuses coordinates from a different frame instead of guessing a transform', () => {
    const result = analyzeSensorSurfaceRelevance(
      Float32Array.from([0, 0, 0]),
      analysis([component(0, [0, 0, 0])], [0]),
      context([0, 0, 0], { sensorFrameId: 'MAIN_LOCAL' }),
      0.04,
    );
    expect(result.status).toBe('frame-mismatch');
    expect(result.relevantComponentIds).toEqual([]);
    expect(result.components).toEqual([]);
  });

  it('marks a component relevant when a locked sensor is near actual measured surfels', () => {
    const positions = Float32Array.from([
      0, 0, 0,
      0.10, 0, 0,
      0.20, 0, 0,
      0.30, 0, 0,
    ]);
    const result = analyzeSensorSurfaceRelevance(
      positions,
      analysis([component(0, [0.15, 0, 0])], [0, 0, 0, 0]),
      context([0.32, 0, 0]),
      0.04,
    );
    expect(result.status).toBe('ready');
    expect(result.relevantComponentIds).toEqual([0]);
    expect(result.components[0].nearestDistanceMeters).toBeCloseTo(0.02, 6);
  });

  it('uses point-to-sensor distance rather than component-centroid distance', () => {
    const positions = Float32Array.from([
      0, 0, 0,
      0.3, 0, 0,
      0.6, 0, 0,
      0.9, 0, 0,
    ]);
    const result = analyzeSensorSurfaceRelevance(
      positions,
      analysis([component(0, [0.45, 0, 0])], [0, 0, 0, 0]),
      context([0.94, 0, 0]),
      0.04,
    );
    expect(Math.abs(0.94 - 0.45)).toBeGreaterThan(0.40);
    expect(result.components[0].nearestDistanceMeters).toBeCloseTo(0.04, 6);
    expect(result.relevantComponentIds).toEqual([0]);
  });

  it('does not turn a far sensor into evidence that the component is invalid', () => {
    const result = analyzeSensorSurfaceRelevance(
      Float32Array.from([0, 0, 0, 0.1, 0, 0]),
      analysis([component(0, [0.05, 0, 0])], [0, 0]),
      context([2, 0, 0]),
      0.04,
    );
    expect(result.status).toBe('ready');
    expect(result.relevantComponentIds).toEqual([]);
    expect(result.components[0].relevant).toBe(false);
    // This analysis carries no deletion/disposition field by design: a negative
    // proximity result cannot erase physical geometry or alter TDOA.
    expect(result.components[0].componentId).toBe(0);
  });

  it('ignores unlocked/provisional sensor coordinates', () => {
    const result = analyzeSensorSurfaceRelevance(
      Float32Array.from([0, 0, 0]),
      analysis([component(0, [0, 0, 0])], [0]),
      context([0, 0, 0], {
        sensors: [{ id: 'G1', position: [0, 0, 0], locked: false }],
      }),
      0.04,
    );
    expect(result.status).toBe('unavailable');
    expect(result.relevantComponentIds).toEqual([]);
  });

  it('uses bounded placement uncertainty to protect a plausible nearby component', () => {
    const source = Float32Array.from([0, 0, 0]);
    const surface = analysis([component(0, [0, 0, 0])], [0]);
    const withoutUncertainty = analyzeSensorSurfaceRelevance(
      source,
      surface,
      context([0.24, 0, 0]),
      0.04,
    );
    const withUncertainty = analyzeSensorSurfaceRelevance(
      source,
      surface,
      context([0.24, 0, 0], {
        sensors: [{ id: 'G1', position: [0.24, 0, 0], locked: true, uncertaintyMeters: 0.10 }],
      }),
      0.04,
    );
    expect(withoutUncertainty.relevantComponentIds).toEqual([]);
    expect(withUncertainty.relevantComponentIds).toEqual([0]);
    expect(withUncertainty.components[0].relevanceGateMeters).toBeLessThanOrEqual(0.55);
  });
});
