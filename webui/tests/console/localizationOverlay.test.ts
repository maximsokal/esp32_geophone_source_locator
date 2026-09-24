import { describe, expect, it } from 'vitest';

import type { LocalizationRun, LocalizationSessionGeometry } from '../../src/api/deviceApi';
import {
  NOMINAL_SECTOR_RADIUS_M,
  buildLocalizationOverlay,
  overlappingSectors,
  sectorRadius,
  selectOverlayMethod,
} from '../../src/domain/localizationOverlay';

/* MAX-161: method comparison overlay model. */

function run(overrides: Partial<LocalizationRun>): LocalizationRun {
  return {
    schema_version: 1,
    object_id: 'obj',
    session_id: 's1',
    computed_at_ms: 1000,
    kind: 'individual_method',
    method_name: 'gauss_newton_tdoa',
    method_version: 1,
    channel_mask_used: 15,
    channel_mask_rejected: 0,
    dimensionality: 'planar_2d',
    geometry_computed_at_ms: 500,
    propagation_model_name: 'constant',
    propagation_speed_mps: 300,
    propagation_speed_uncertainty_mps: 20,
    confidence_score: 0.8,
    solved: true,
    source_position: { x_m: 1, y_m: 2, z_m: 0 },
    rms_residual_us: 1000,
    ...overrides,
  };
}

const geometry: LocalizationSessionGeometry = {
  available: true,
  pods: [
    { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
    { pod: 1, computed: true, leveled: true, x_mm: 5000, y_mm: 0, z_mm: 120 },
    { pod: 2, computed: false, leveled: false },
  ],
};

describe('localization overlay model', () => {
  const runs = [
    run({ method_name: 'gauss_newton_tdoa', source_position: { x_m: 1, y_m: 2, z_m: 0 } }),
    run({ method_name: 'semblance_scan', source_position: { x_m: 4, y_m: -1, z_m: -0.5 }, rms_residual_us: undefined }),
    run({ method_name: 'traveltime_inversion', solved: false, source_position: undefined, rejection_reason: 'geometry is collinear' }),
  ];

  it('builds one sector per solved method and lists failures with their reason', () => {
    const model = buildLocalizationOverlay(runs, geometry, null);
    expect(model.sectors.map((s) => s.method)).toEqual(['gauss_newton_tdoa', 'semblance_scan']);
    expect(model.unavailable).toEqual([{ method: 'traveltime_inversion', reason: 'geometry is collinear', computedAtMs: 1000 }]);
    /* No fabricated origin sector for the failed method. */
    expect(model.sectors.some((s) => s.position.x_m === 0 && s.position.y_m === 0 && s.position.z_m === 0)).toBe(false);
    /* Sectors differ. */
    expect(model.sectors[0].position).not.toEqual(model.sectors[1].position);
    expect(model.sensors.map((s) => s.label)).toEqual(['MAIN', 'Pod 1']);
    expect(model.sensors[1].position).toEqual({ x_m: 5, y_m: 0, z_m: 0.12 });
  });

  it('defaults the selection to the first solved method and emphasises only it', () => {
    const model = buildLocalizationOverlay(runs, geometry, 'traveltime_inversion');
    expect(model.selectedMethod).toBe('gauss_newton_tdoa');
    expect(model.sectors.map((s) => s.emphasis)).toEqual(['selected', 'dim']);
  });

  it('changing the selection changes emphasis only, never coordinates or radii', () => {
    const before = buildLocalizationOverlay(runs, geometry, 'gauss_newton_tdoa');
    const after = selectOverlayMethod(before, 'semblance_scan');
    expect(after.selectedMethod).toBe('semblance_scan');
    expect(after.sectors.map((s) => s.emphasis)).toEqual(['dim', 'selected']);
    expect(after.sectors.map((s) => [s.position, s.radiusM])).toEqual(before.sectors.map((s) => [s.position, s.radiusM]));
    /* Unknown method: unchanged. */
    expect(selectOverlayMethod(after, 'traveltime_inversion')).toBe(after);
  });

  it('uses only the latest run of a method', () => {
    const model = buildLocalizationOverlay([
      run({ method_name: 'gauss_newton_tdoa', computed_at_ms: 1, source_position: { x_m: 9, y_m: 9, z_m: 9 } }),
      run({ method_name: 'gauss_newton_tdoa', computed_at_ms: 2, source_position: { x_m: 1, y_m: 1, z_m: 1 } }),
    ], geometry, null);
    expect(model.sectors).toHaveLength(1);
    expect(model.sectors[0].position).toEqual({ x_m: 1, y_m: 1, z_m: 1 });
  });

  it('derives the sector radius from the residual, or marks it nominal', () => {
    expect(sectorRadius(run({ rms_residual_us: 1000, propagation_speed_mps: 300 }))).toEqual({ radiusM: 0.3, source: 'residual' });
    expect(sectorRadius(run({ rms_residual_us: undefined }))).toEqual({ radiusM: NOMINAL_SECTOR_RADIUS_M, source: 'nominal' });
    expect(sectorRadius(run({ rms_residual_us: 1e12 })).radiusM).toBe(50);
    expect(sectorRadius(run({ rms_residual_us: 1 })).radiusM).toBe(0.05);
  });

  it('reports overlapping sectors so they can be told apart', () => {
    const model = buildLocalizationOverlay([
      run({ method_name: 'a', source_position: { x_m: 0, y_m: 0, z_m: 1 }, rms_residual_us: 1000 }),
      run({ method_name: 'b', source_position: { x_m: 0.4, y_m: 0, z_m: 1 }, rms_residual_us: 1000 }),
      run({ method_name: 'c', source_position: { x_m: 10, y_m: 0, z_m: 1 }, rms_residual_us: 1000 }),
    ], geometry, null);
    expect(overlappingSectors(model)).toEqual([['a', 'b']]);
  });

  it('ignores non-finite positions and missing geometry', () => {
    const model = buildLocalizationOverlay([
      run({ method_name: 'broken', source_position: { x_m: Number.NaN, y_m: 0, z_m: 0 } }),
    ], { available: false, pods: [] }, null);
    expect(model.sectors).toHaveLength(0);
    expect(model.unavailable[0].method).toBe('broken');
    expect(model.selectedMethod).toBeNull();
    expect(model.sensors).toHaveLength(0);
  });
});
