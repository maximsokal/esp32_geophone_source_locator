import { describe, expect, it } from 'vitest';

import type { LocalizationSessionGeometry } from '../../src/api/deviceApi';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import { buildSensorPlacementScene, type SensorPlacementNode } from '../../src/xr/sensorPlacementScene';
import { prepareScanScene, scanLayerPlan, toViewport } from '../../src/xr/scanViewportScene';
import { displayForScanMode } from '../../src/xr/scanDisplayModes';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

function geometry(pods: LocalizationSessionGeometry['pods']): LocalizationSessionGeometry {
  return { available: true, pods };
}

function pod(nodeId: number, overrides: Partial<LocalizationSessionGeometry['pods'][number]> = {}) {
  return {
    pod: nodeId,
    node_id: nodeId,
    computed: true,
    assigned: true,
    leveled: true,
    position_source: 'channel_sounding' as const,
    x_mm: nodeId * 1000,
    y_mm: nodeId * 2000,
    z_mm: nodeId * 3000,
    uncertainty_mm: 40,
    residual_mm: 50,
    ...overrides,
  };
}

function binding(node: SensorPlacementNode, tagId: number, position: [number, number, number] = [0.1, 0.2, 0.3]) {
  return { tag_id: tagId, node, observation_count: 5, inlier_count: 5, position_scan_m: position, spread_m: 0.006 };
}

function artifact(overrides: Partial<RegistrationArtifact> = {}): RegistrationArtifact {
  return {
    schema: 1,
    source: 'apriltag',
    family: 'tag36h11',
    geometry_computed_at_ms: 100,
    created_at_ms: 101,
    tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
    bindings: [],
    sensor_to_scan: IDENTITY,
    mirrored: false,
    residual_rms_m: 0.002,
    threshold_m: 0.01,
    per_node: [],
    tier: 'minimum',
    valid: true,
    reason: null,
    ...overrides,
  };
}

describe('sensor placement scene', () => {
  it('builds anchored, CS-projected, visual-only and unregistered markers', () => {
    const sensorGeometry = geometry([pod(0), pod(1), pod(2)]);
    const valid = artifact({
      bindings: [binding('G1', 1)],
      per_node: [{ node: 'G1', tagId: 1, residualM: 0.007, status: 'anchored' }],
    });
    const placed = buildSensorPlacementScene(sensorGeometry, valid).markers;
    expect(placed.find((marker) => marker.node === 'G1')).toMatchObject({
      status: 'anchored', source: 'apriltag', tagId: 1, residualM: 0.007,
    });
    expect(placed.find((marker) => marker.node === 'G2')?.status).toBe('cs_projected');

    const invalid = artifact({
      sensor_to_scan: null,
      tier: null,
      valid: false,
      reason: 'missing_main',
      bindings: [binding('G1', 1)],
      per_node: [{ node: 'G1', tagId: 1, residualM: 0.007, status: 'visual_only' }],
    });
    const unregistered = buildSensorPlacementScene(sensorGeometry, invalid).markers;
    expect(unregistered.find((marker) => marker.node === 'G1')).toMatchObject({
      status: 'visual_only', source: 'apriltag', positionScanM: [0.1, -0.3, 0.2], uncertaintyM: 0.006, tagId: 1,
    });
    expect(unregistered.find((marker) => marker.node === 'G2')).toMatchObject({
      status: 'cs_unregistered', positionScanM: null, source: 'channel_sounding',
    });
  });

  it('converts an unregistered local-floor tag marker to the project frame', () => {
    const invalid = artifact({
      sensor_to_scan: null,
      tier: null,
      valid: false,
      reason: 'missing_main',
      bindings: [binding('G1', 1, [1, 2, 3])],
      per_node: [{ node: 'G1', tagId: 1, residualM: null, status: 'visual_only' }],
    });
    const marker = buildSensorPlacementScene(geometry([]), invalid).markers.find((item) => item.node === 'G1');
    expect(marker?.positionScanM).toEqual([1, -3, 2]);
  });

  it.each([
    {
      label: 'proper rotation',
      matrix: [0, -1, 0, 0.4, 1, 0, 0, -0.2, 0, 0, 1, 0.3, 0, 0, 0, 1],
      mirrored: false,
      expected: [-1.6, -3.3, 0.8],
    },
    {
      label: 'reflection',
      matrix: [1, 0, 0, 0.4, 0, 1, 0, -0.2, 0, 0, -1, 0.3, 0, 0, 0, 1],
      mirrored: true,
      expected: [1.4, 2.7, 1.8],
    },
  ])('uses the exact mm -> local-floor -> project chain with $label', ({ matrix, mirrored, expected }) => {
    const result = buildSensorPlacementScene(geometry([pod(1)]), artifact({
      sensor_to_scan: matrix,
      mirrored,
      bindings: [binding('G1', 1)],
    }));
    const position = result.markers.find((marker) => marker.node === 'G1')!.positionScanM!;
    position.forEach((value, axis) => expect(Math.abs(value - expected[axis])).toBeLessThanOrEqual(1e-9));
  });

  it('uses the same centered viewport mapping as a scan point at that location', () => {
    const marker = buildSensorPlacementScene(geometry([pod(1)]), artifact({
      sensor_to_scan: IDENTITY,
      bindings: [binding('G1', 1)],
    })).markers.find((item) => item.node === 'G1')!;
    const point: [number, number, number] = [...marker.positionScanM!] as [number, number, number];
    const bounds = { min: [0, 0, 0] as const, max: [2, 4, 6] as const };
    const prepared = prepareScanScene({
      bounds,
      positions: new Float32Array(point),
      confidence: new Float32Array([0.9]),
      tentativePositions: new Float32Array(0),
      tentativeConfidence: new Float32Array(0),
      manualControlPoints: [],
      operatorEdges: [],
      freeAirPositions: new Float32Array(0),
      unknownCandidatePositions: new Float32Array(0),
    } as ScanPointCloudSnapshot);
    const pointViewport: [number, number, number] = Array.from(prepared.good.stable) as [number, number, number];
    const markerViewport = toViewport(point[0], point[1], point[2], prepared.centerX, prepared.centerY, prepared.centerZ);
    markerViewport.forEach((value, axis) => expect(Math.abs(value - pointViewport[axis])).toBeLessThanOrEqual(1e-9));
  });

  it('preserves barometric Channel Sounding provenance for sensor placement', () => {
    const marker = buildSensorPlacementScene(
      geometry([pod(1, { position_source: 'channel_sounding_baro' })]), null,
    ).markers.find((item) => item.node === 'G1')!;
    expect(marker).toMatchObject({ status: 'cs_unregistered', source: 'channel_sounding_baro' });
  });

  it('reports missing, partial, unassigned, uncomputed and non-finite geometry', () => {
    expect(buildSensorPlacementScene(null, null).markers).toEqual([]);
    expect(buildSensorPlacementScene(null, null).skipped).toEqual(NODES.map((node) => ({ node, reason: 'missing_geometry' })));

    const result = buildSensorPlacementScene(geometry([
      pod(1, { z_mm: undefined }),
      pod(2, { assigned: false }),
      pod(3, { computed: false }),
      pod(4, { x_mm: Number.NaN }),
    ]), null);
    expect(result.markers.find((marker) => marker.node === 'MAIN')?.status).toBe('cs_unregistered');
    expect(result.skipped).toEqual(expect.arrayContaining([
      { node: 'G1', reason: 'missing_or_non_finite_geometry' },
      { node: 'G2', reason: 'not_assigned' },
      { node: 'G3', reason: 'not_computed' },
      { node: 'G4', reason: 'missing_or_non_finite_geometry' },
    ]));
  });

  it('is deterministic when a saved session is reopened with identical inputs', () => {
    const savedGeometry = geometry([pod(0), pod(1)]);
    const savedRegistration = artifact({ bindings: [binding('G1', 1)] });
    expect(buildSensorPlacementScene(savedGeometry, savedRegistration))
      .toEqual(buildSensorPlacementScene(savedGeometry, savedRegistration));
  });

  it('applies a registration only when it is bound to the viewed scan and geometry generation (MAX-405)', () => {
    const sensorGeometry = geometry([pod(0), pod(1)]);
    const bound = artifact({ scan_frame_id: 'scan-crc32:aaaaaaaa', bindings: [binding('G1', 1)], per_node: [{ node: 'G1', tagId: 1, residualM: 0.007, status: 'anchored' }] });
    const expected = { geometryComputedAtMs: 100, scanFrameId: 'scan-crc32:aaaaaaaa' };
    expect(buildSensorPlacementScene(sensorGeometry, bound, expected).markers.find((m) => m.node === 'G1')?.status).toBe('anchored');
    for (const [other, reason] of [
      [{ ...expected, scanFrameId: 'scan-crc32:bbbbbbbb' }, 'registration_scan_mismatch'],
      [{ ...expected, geometryComputedAtMs: 101 }, 'registration_geometry_mismatch'],
    ] as const) {
      const stale = buildSensorPlacementScene(sensorGeometry, bound, other);
      expect(stale.markers.find((m) => m.node === 'G1')?.status).toBe('cs_unregistered');
      expect(stale.skipped).toContainEqual({ node: null, reason });
    }
    const unbound = buildSensorPlacementScene(sensorGeometry, artifact({ bindings: [binding('G1', 1)] }), expected);
    expect(unbound.markers.every((m) => m.status === 'cs_unregistered')).toBe(true);
    expect(unbound.skipped).toContainEqual({ node: null, reason: 'registration_scan_unbound' });
  });

  it('refuses a transform whose residual exceeds its threshold', () => {
    const over = artifact({ residual_rms_m: 0.3, threshold_m: 0.05 });
    expect(buildSensorPlacementScene(geometry([pod(0), pod(1)]), over).markers.every((m) => m.status === 'cs_unregistered')).toBe(true);
  });
});

const NODES: SensorPlacementNode[] = ['MAIN', 'G1', 'G2', 'G3', 'G4'];
