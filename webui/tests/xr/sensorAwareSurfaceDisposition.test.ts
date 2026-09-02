import { describe, expect, it } from 'vitest';

import { classifySensorAwareSurfaceComponents } from '../../src/xr/sensorAwareSurfaceDisposition';
import type { SensorSurfaceRelevanceAnalysis } from '../../src/xr/sensorAwareSurfaceRelevance';
import type { OperationalSurfaceEnvelope } from '../../src/xr/surfaceOperationalEnvelope';

function envelope(rescanBridgeComponentIds: readonly number[] = []): OperationalSurfaceEnvelope {
  return {
    bounds: null,
    retainedComponentIds: [0, 2],
    quarantinedComponentIds: [1, 3],
    rescanBridgeComponentIds,
    bridgeEvidenceTruncated: false,
    retainedPointCount: 100,
    quarantinedPointCount: 20,
  };
}

function relevance(status: SensorSurfaceRelevanceAnalysis['status']): SensorSurfaceRelevanceAnalysis {
  return {
    status,
    relevantComponentIds: status === 'ready' ? [1, 2] : [],
    components: status === 'ready'
      ? [
          {
            componentId: 1,
            nearestSensorId: 'G1',
            nearestDistanceMeters: 0.08,
            relevanceGateMeters: 0.16,
            relevant: true,
          },
          {
            componentId: 2,
            nearestSensorId: 'G2',
            nearestDistanceMeters: 0.05,
            relevanceGateMeters: 0.16,
            relevant: true,
          },
          {
            componentId: 3,
            nearestSensorId: 'G3',
            nearestDistanceMeters: 0.45,
            relevanceGateMeters: 0.16,
            relevant: false,
          },
        ]
      : [],
  };
}

describe('sensor-aware floating island disposition', () => {
  it('keeps physically retained components retained even near a sensor', () => {
    const result = classifySensorAwareSurfaceComponents(envelope(), relevance('ready'));
    expect(result.find((entry) => entry.componentId === 2)?.disposition).toBe('retain');
  });

  it('promotes only a quarantined component with frame-validated positive sensor relevance', () => {
    const result = classifySensorAwareSurfaceComponents(envelope(), relevance('ready'));
    const promoted = result.find((entry) => entry.componentId === 1);
    expect(promoted?.disposition).toBe('promote-near-sensor');
    expect(promoted?.nearestSensorId).toBe('G1');
    expect(promoted?.nearestSensorDistanceMeters).toBeCloseTo(0.08);
    expect(result.find((entry) => entry.componentId === 3)?.disposition).toBe('quarantine');
  });

  it('marks a sensor-neutral weak bridge only as rescan metadata', () => {
    const result = classifySensorAwareSurfaceComponents(envelope([3]), null);
    expect(result.find((entry) => entry.componentId === 3)?.disposition).toBe('rescan-bridge');
    expect(result.find((entry) => entry.componentId === 1)?.disposition).toBe('quarantine');
  });

  it('lets frame-validated sensor relevance outrank bridge-rescan metadata without certifying geometry', () => {
    const result = classifySensorAwareSurfaceComponents(envelope([1]), relevance('ready'));
    expect(result.find((entry) => entry.componentId === 1)?.disposition).toBe('promote-near-sensor');
  });

  it('never promotes on a coordinate-frame mismatch', () => {
    const result = classifySensorAwareSurfaceComponents(envelope(), relevance('frame-mismatch'));
    expect(result.find((entry) => entry.componentId === 1)?.disposition).toBe('quarantine');
    expect(result.find((entry) => entry.componentId === 3)?.disposition).toBe('quarantine');
  });

  it('frame mismatch still allows sensor-neutral bridge RESCAN but never sensor promotion', () => {
    const result = classifySensorAwareSurfaceComponents(envelope([3]), relevance('frame-mismatch'));
    expect(result.find((entry) => entry.componentId === 3)?.disposition).toBe('rescan-bridge');
    expect(result.filter((entry) => entry.disposition === 'promote-near-sensor')).toHaveLength(0);
  });

  it('never promotes when sensor coordinates are unavailable', () => {
    const result = classifySensorAwareSurfaceComponents(envelope(), null);
    expect(result.filter((entry) => entry.disposition === 'promote-near-sensor')).toHaveLength(0);
  });

  it('does not invent components that are absent from the physical envelope policy', () => {
    const ready = relevance('ready');
    const withUnknown = {
      ...ready,
      relevantComponentIds: [...ready.relevantComponentIds, 99],
      components: [
        ...ready.components,
        {
          componentId: 99,
          nearestSensorId: 'G4',
          nearestDistanceMeters: 0.01,
          relevanceGateMeters: 0.16,
          relevant: true,
        },
      ],
    } satisfies SensorSurfaceRelevanceAnalysis;
    const result = classifySensorAwareSurfaceComponents(envelope(), withUnknown);
    expect(result.some((entry) => entry.componentId === 99)).toBe(false);
  });
});
