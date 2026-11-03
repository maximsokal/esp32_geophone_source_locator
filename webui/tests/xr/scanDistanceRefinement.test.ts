import { describe, expect, it } from 'vitest';

import type { LocalizationSessionGeometry } from '../../src/api/deviceApi';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import { buildSensorPlacementScene } from '../../src/xr/sensorPlacementScene';
import { computeScanDistanceRefinement, scannedNodes, SCAN_SCALE_UNCERTAINTY_FRACTION } from '../../src/xr/scanDistanceRefinement';

type Pod = LocalizationSessionGeometry['pods'][number];

function pod(nodeId: number, x: number, y: number, z = 0, overrides: Partial<Pod> = {}): Pod {
  return { pod: nodeId, node_id: nodeId, computed: true, assigned: true, leveled: true, x_mm: x, y_mm: y, z_mm: z, ...overrides };
}

/** MAIN at the origin, G1 3 m along x, G2 4 m along y: Channel Sounding distances 3.0 / 4.0 / 5.0 m. */
const GEOMETRY: LocalizationSessionGeometry = { available: true, pods: [pod(0, 0, 0), pod(1, 3000, 0), pod(2, 0, 4000)] };

function binding(
  node: 'MAIN' | 'G1' | 'G2' | 'G3' | 'G4',
  tagId: number,
  position: [number, number, number],
  overrides: Partial<RegistrationArtifact['bindings'][number]> = {},
): RegistrationArtifact['bindings'][number] {
  return { tag_id: tagId, node, observation_count: 9, inlier_count: 8, position_scan_m: position, spread_m: 0.01, ...overrides };
}

function artifact(bindings: RegistrationArtifact['bindings']): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: 'scan-crc32:00000001', source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: 1, created_at_ms: 2, tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
    bindings, sensor_to_scan: null, mirrored: false, residual_rms_m: 0, threshold_m: 0.05, per_node: [], tier: null, valid: false,
    reason: 'missing_main',
  };
}

const SCAN = artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [2.8, 0, 0]), binding('G2', 2, [0, 4.1, 0])]);
const rms = (values: number[]) => Math.sqrt(values.reduce((sum, value) => sum + value * value, 0) / values.length);

describe('distances recalculated from the scan', () => {
  it('compares every scanned pair with the Channel Sounding distance and reports the error reduction', () => {
    const result = computeScanDistanceRefinement(GEOMETRY, SCAN);
    expect(result.status).toBe('ok');
    if (result.status !== 'ok') return;
    expect(result.scannedNodes).toEqual(['MAIN', 'G1', 'G2']);
    expect(result.pairs.map((pair) => `${pair.a}-${pair.b}`)).toEqual(['MAIN-G1', 'MAIN-G2', 'G1-G2']);

    const g1 = result.pairs[0];
    expect(g1.csMm).toBeCloseTo(3000, 6);
    expect(g1.scanMm).toBeCloseTo(2800, 6);
    expect(g1.correctionMm).toBeCloseTo(-200, 6);
    expect(g1.beforeMm).toBeCloseTo(200, 6);
    expect(g1.afterMm).toBeCloseTo(Math.sqrt(10 ** 2 + 10 ** 2 + (SCAN_SCALE_UNCERTAINTY_FRACTION * 2800) ** 2), 6);

    const diagonal = Math.hypot(2800, 4100);
    const diagonalPair = result.pairs[2];
    expect(diagonalPair.scanMm).toBeCloseTo(diagonal, 6);
    expect(diagonalPair.beforeMm).toBeCloseTo(5000 - diagonal, 6);
    /* This pair was already closer to the scan than the scan is sure of itself: no reduction for it alone. */
    expect(diagonalPair.reductionPercent).toBe(0);

    const before = [200, 100, 5000 - diagonal];
    const after = [Math.sqrt(200 + 28 ** 2), Math.sqrt(200 + 41 ** 2), Math.sqrt(200 + (0.01 * diagonal) ** 2)];
    expect(result.summary.pairCount).toBe(3);
    expect(result.summary.rmsBeforeMm).toBeCloseTo(rms(before), 6);
    expect(result.summary.rmsAfterMm).toBeCloseTo(rms(after), 6);
    expect(result.summary.reductionPercent).toBeCloseTo((1 - rms(after) / rms(before)) * 100, 6);
    expect(result.summary.reductionPercent!).toBeGreaterThan(60);
    expect(result.summary.reductionPercent!).toBeLessThan(75);
  });

  it('only counts the devices that were actually scanned, and only with enough observations', () => {
    const partial = artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [2.8, 0, 0]), binding('G2', 2, [0, 4.1, 0], { inlier_count: 4 })]);
    expect(scannedNodes(partial)).toEqual(['MAIN', 'G1']);
    const result = computeScanDistanceRefinement(GEOMETRY, partial);
    expect(result.status === 'ok' && result.pairs.map((pair) => `${pair.a}-${pair.b}`)).toEqual(['MAIN-G1']);
    expect(scannedNodes(null)).toEqual([]);
    const nonFinite = artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [Number.NaN, 0, 0])]);
    expect(scannedNodes(nonFinite)).toEqual(['MAIN']);
  });

  it('says why nothing could be recalculated', () => {
    expect(computeScanDistanceRefinement(GEOMETRY, null)).toMatchObject({ status: 'unavailable', reason: 'no_registration', scannedNodes: [] });
    expect(computeScanDistanceRefinement(GEOMETRY, artifact([]))).toMatchObject({ status: 'unavailable', reason: 'no_registration' });
    expect(computeScanDistanceRefinement(GEOMETRY, artifact([binding('G1', 1, [1, 0, 0])])))
      .toMatchObject({ status: 'unavailable', reason: 'too_few_devices', scannedNodes: ['G1'] });
    expect(computeScanDistanceRefinement(null, SCAN)).toMatchObject({ status: 'unavailable', reason: 'no_geometry' });
    expect(computeScanDistanceRefinement({ available: false, pods: [] }, SCAN)).toMatchObject({ status: 'unavailable', reason: 'no_geometry' });
    const noPositions: LocalizationSessionGeometry = {
      available: true,
      pods: [pod(1, 0, 0, 0, { computed: false }), pod(2, 0, 0, 0, { assigned: false })],
    };
    expect(computeScanDistanceRefinement(noPositions, artifact([binding('G1', 1, [0, 0, 0]), binding('G2', 2, [1, 0, 0])])))
      .toMatchObject({ status: 'unavailable', reason: 'no_common_pairs' });
  });

  it('skips a scanned device that has no Channel Sounding position and says so', () => {
    const scan = artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [2.8, 0, 0]), binding('G3', 3, [5, 5, 0])]);
    const result = computeScanDistanceRefinement(GEOMETRY, scan);
    expect(result.status).toBe('ok');
    if (result.status !== 'ok') return;
    expect(result.skippedNodes).toEqual(['G3']);
    expect(result.pairs.map((item) => `${item.a}-${item.b}`)).toEqual(['MAIN-G1']);
  });

  it('reports no reduction when the distances already agree with the scan', () => {
    const exact = computeScanDistanceRefinement(GEOMETRY, artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [3, 0, 0])]));
    expect(exact.status === 'ok' && exact.summary.reductionPercent).toBeNull();
    const nearly = computeScanDistanceRefinement(GEOMETRY, artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [3.001, 0, 0])]));
    expect(nearly.status === 'ok' && nearly.summary.reductionPercent).toBe(0);
  });

  it('separates the offset every pair shares from what a constant offset does not explain', () => {
    /* Channel Sounding reads 300 mm long on every pair; the scan sees 2.7 / 3.7 / hypot. A pure offset leaves no spread. */
    const shifted = artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [2.7, 0, 0]), binding('G2', 2, [0, 3.7, 0])]);
    const result = computeScanDistanceRefinement(GEOMETRY, shifted);
    expect(result.status).toBe('ok');
    if (result.status !== 'ok') return;
    const diagonal = Math.hypot(2700, 3700);
    const errors = [3000 - 2700, 4000 - 3700, 5000 - diagonal];
    expect(result.summary.offsetMm).toBeCloseTo(300, 6);                      /* median of 300, 300, 5000 - 4580.4 */
    expect(result.summary.spreadAfterOffsetMm).toBeCloseTo(Math.sqrt(errors.reduce((sum, e) => sum + (e - 300) ** 2, 0) / 3), 6);
    expect(result.summary.offsetReliable).toBe(true);

    const two = computeScanDistanceRefinement(GEOMETRY, artifact([binding('MAIN', 0, [0, 0, 0]), binding('G1', 1, [2.7, 0, 0])]));
    expect(two.status === 'ok' && two.summary.offsetReliable).toBe(false);     /* one pair is not a calibration */
    expect(two.status === 'ok' && two.summary.offsetMm).toBeCloseTo(300, 6);
    const exact = computeScanDistanceRefinement(GEOMETRY, SCAN);
    expect(exact.status === 'ok' && exact.summary.offsetMm).toBeCloseTo(5000 - Math.hypot(2800, 4100), 6);   /* median of 200, -100 and 35.1 */
  });

  it('does not depend on the frame the scan is in: only distances are compared', () => {
    const moved = artifact([binding('MAIN', 0, [10, 5, 1]), binding('G1', 1, [10, 5 + 2.8, 1]), binding('G2', 2, [10 - 4.1, 5, 1])]);
    const a = computeScanDistanceRefinement(GEOMETRY, SCAN);
    const b = computeScanDistanceRefinement(GEOMETRY, moved);
    expect(a.status).toBe('ok');
    expect(b.status === 'ok' && b.summary.reductionPercent).toBeCloseTo(a.status === 'ok' ? a.summary.reductionPercent! : -1, 6);
  });

  it('every scanned device is also a marker in the 3D scan scene, even when no registration transform exists', () => {
    const scene = buildSensorPlacementScene(GEOMETRY, SCAN);
    const scanned = scene.markers.filter((marker) => marker.source === 'apriltag');
    expect(scanned.map((marker) => marker.node)).toEqual(['MAIN', 'G1', 'G2']);
    expect(scanned.every((marker) => marker.status === 'visual_only' && marker.positionScanM !== null)).toBe(true);
  });
});
