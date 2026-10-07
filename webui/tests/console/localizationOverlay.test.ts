import { describe, expect, it } from 'vitest';

import type { LocalizationRun, LocalizationSessionGeometry } from '../../src/api/deviceApi';
import {
  NOMINAL_SECTOR_RADIUS_M,
  buildLocalizationOverlay,
  classifyRunCurrency,
  credibleBox,
  dimensionalityLabel,
  filterOverlayByVariant,
  isConstrainedMethod,
  isPlanarSector,
  overlappingSectors,
  resultVariant,
  sectorRadius,
  selectOverlayMethod,
  toggleAvailability,
  uncertaintyText,
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
    expect(model.unavailable).toEqual([
      { method: 'traveltime_inversion', reason: 'geometry is collinear', computedAtMs: 1000, variant: 'unconstrained' },
    ]);
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

  it('geometry === null (not merely unavailable) also yields no sensors', () => {
    const model = buildLocalizationOverlay(runs, null, null);
    expect(model.sensors).toHaveLength(0);
  });

  it('no runs at all: no sectors, no unavailable entries, nothing selected', () => {
    const model = buildLocalizationOverlay([], geometry, null);
    expect(model.sectors).toHaveLength(0);
    expect(model.unavailable).toHaveLength(0);
    expect(model.selectedMethod).toBeNull();
  });

  it('only failed/unsolved runs: no markers at all, every one reported with its reason', () => {
    const model = buildLocalizationOverlay([
      run({ method_name: 'traveltime_inversion', solved: false, source_position: undefined, rejection_reason: 'fewer than 4 accepted picks' }),
      run({ method_name: 'gauss_newton_tdoa', solved: true, source_position: undefined }),
    ], geometry, null);
    expect(model.sectors).toHaveLength(0);
    expect(model.unavailable.map((u) => u.method)).toEqual(['gauss_newton_tdoa', 'traveltime_inversion']);
    expect(model.unavailable.find((u) => u.method === 'traveltime_inversion')?.reason).toBe('fewer than 4 accepted picks');
    /* solved:true but no position is still reported, never placed at the origin. */
    expect(model.unavailable.find((u) => u.method === 'gauss_newton_tdoa')?.reason).toBe('no source position reported');
    expect(model.selectedMethod).toBeNull();
  });
});

describe('dimensionality: planar (3-REMOTE) vs 3D', () => {
  it('classifies planar_2d as planar and capable_3d/insufficient as not', () => {
    expect(isPlanarSector({ dimensionality: 'planar_2d' })).toBe(true);
    expect(isPlanarSector({ dimensionality: 'capable_3d' })).toBe(false);
    expect(isPlanarSector({ dimensionality: 'insufficient' })).toBe(false);
  });

  it('labels dimensionality for display', () => {
    expect(dimensionalityLabel('planar_2d')).toBe('2D');
    expect(dimensionalityLabel('capable_3d')).toBe('3D');
    expect(dimensionalityLabel('insufficient')).toBe('insufficient');
  });

  it('a planar_2d run carries its dimensionality straight through onto the sector, unchanged', () => {
    const model = buildLocalizationOverlay([
      run({ method_name: 'gauss_newton_tdoa', dimensionality: 'planar_2d', source_position: { x_m: 1, y_m: 1, z_m: 0.5 } }),
      run({ method_name: 'eikonal_fmm', dimensionality: 'capable_3d', source_position: { x_m: 2, y_m: 2, z_m: -3 } }),
    ], geometry, null);
    expect(model.sectors.find((s) => s.method === 'gauss_newton_tdoa')?.dimensionality).toBe('planar_2d');
    expect(model.sectors.find((s) => s.method === 'eikonal_fmm')?.dimensionality).toBe('capable_3d');
    /* MAX-148: the planar run's Z is not fabricated as a solved coordinate -- the model still carries
     * whatever the backend reported (a nominal Z), but the sector is flagged planar so the scene draws
     * a flat disc rather than a positioned sphere. */
    expect(isPlanarSector(model.sectors.find((s) => s.method === 'gauss_newton_tdoa')!)).toBe(true);
    expect(isPlanarSector(model.sectors.find((s) => s.method === 'eikonal_fmm')!)).toBe(false);
  });
});

describe('uncertainty: missing or NaN', () => {
  it('sectorRadius falls back to nominal when uncertainty is absent', () => {
    expect(sectorRadius(run({ uncertainty: undefined, rms_residual_us: undefined })))
      .toEqual({ radiusM: NOMINAL_SECTOR_RADIUS_M, source: 'nominal' });
  });

  it('sectorRadius ignores a NaN/zero posterior std and falls back to the residual', () => {
    const withNanStd = run({
      rms_residual_us: 1000,
      propagation_speed_mps: 300,
      uncertainty: {
        std_m: { x_m: Number.NaN, y_m: 0, z_m: 0 },
        credible_level: 0.9,
        credible_volume_m3: 1,
        credible_area_m2: 0,
        credible_min: { x_m: -1, y_m: -1, z_m: -1 },
        credible_max: { x_m: 1, y_m: 1, z_m: 1 },
      },
    });
    /* NaN std disqualifies the posterior region; falls back to the residual-derived radius. */
    expect(sectorRadius(withNanStd)).toEqual({ radiusM: 0.3, source: 'residual' });
    expect(credibleBox(withNanStd)).toBeNull();
  });

  it('uncertaintyText is blank when uncertainty is missing or its credible_level is 0/NaN', () => {
    expect(uncertaintyText({ uncertainty: undefined })).toBe('');
    expect(uncertaintyText({
      uncertainty: {
        std_m: { x_m: 0.1, y_m: 0.1, z_m: 0.1 },
        credible_level: Number.NaN,
        credible_volume_m3: 1,
        credible_area_m2: 0,
        credible_min: { x_m: -1, y_m: -1, z_m: -1 },
        credible_max: { x_m: 1, y_m: 1, z_m: 1 },
      },
    })).toBe('');
  });
});

describe('constrained vs unconstrained method names (MAX-147)', () => {
  it('recognises every backend constrained-suffix as constrained, and the base solve as unconstrained', () => {
    expect(isConstrainedMethod('traveltime_scan_constrained')).toBe(true);
    expect(isConstrainedMethod('semblance_scan_constrained')).toBe(true);
    expect(isConstrainedMethod('tdcas_scan_constrained')).toBe(true);
    expect(isConstrainedMethod('psafuvs_scan_constrained')).toBe(true);
    expect(isConstrainedMethod('suea_scan_voids')).toBe(true);
    expect(isConstrainedMethod('eikonal_scan_voids')).toBe(true);
    expect(isConstrainedMethod('bayesian_scan_voids')).toBe(true);
    expect(isConstrainedMethod('semblance_eikonal_voids')).toBe(true);
    expect(isConstrainedMethod('traveltime_inversion')).toBe(false);
    expect(isConstrainedMethod('gauss_newton_tdoa')).toBe(false);
    expect(isConstrainedMethod('joint_fusion')).toBe(false);
  });

  it('resultVariant mirrors isConstrainedMethod', () => {
    expect(resultVariant('eikonal_scan_voids')).toBe('scan_constrained');
    expect(resultVariant('eikonal_fmm')).toBe('unconstrained');
  });

  it('every built sector/unavailable entry carries its variant', () => {
    const model = buildLocalizationOverlay([
      run({ method_name: 'gauss_newton_tdoa', source_position: { x_m: 1, y_m: 1, z_m: 0 } }),
      run({ method_name: 'traveltime_scan_constrained', source_position: { x_m: 1.1, y_m: 1.1, z_m: 0 } }),
      run({ method_name: 'bayesian_scan_voids', solved: false, source_position: undefined, rejection_reason: 'no scan registered' }),
    ], geometry, null);
    expect(model.sectors.find((s) => s.method === 'gauss_newton_tdoa')?.variant).toBe('unconstrained');
    expect(model.sectors.find((s) => s.method === 'traveltime_scan_constrained')?.variant).toBe('scan_constrained');
    expect(model.unavailable.find((u) => u.method === 'bayesian_scan_voids')?.variant).toBe('scan_constrained');
  });
});

describe('localization run scan currency (MAX-414)', () => {
  it('treats a constrained run as current only for the scan it actually constrained', () => {
    const constrained = run({
      method_name: 'traveltime_scan_constrained',
      scan_constraint_applied: true,
      scan_frame_id: 'scan-crc32:continue-a',
    });
    expect(classifyRunCurrency(constrained, 'scan-crc32:continue-a')).toBe('current');
    // Continue creates a new whole-file scan identity, so the pre-merge run becomes history.
    expect(classifyRunCurrency(constrained, 'scan-crc32:continue-b')).toBe('history_previous_scan');
    expect(classifyRunCurrency({ ...constrained, scan_constraint_applied: false }, 'scan-crc32:continue-a'))
      .toBe('history_previous_scan');
  });

  it('labels legacy constrained runs as unknown-generation and leaves unconstrained runs independent', () => {
    expect(classifyRunCurrency(run({ method_name: 'bayesian_scan_voids' }), 'scan-crc32:current'))
      .toBe('history_scan_unknown');
    expect(classifyRunCurrency(run({ method_name: 'gauss_newton_tdoa' }), 'scan-crc32:current'))
      .toBe('unconstrained');
  });
});

describe('toggle availability (MAX-148 unconstrained vs scan-constrained)', () => {
  const unconstrainedOnly = [run({ method_name: 'gauss_newton_tdoa' })];
  const mixed = [
    run({ method_name: 'gauss_newton_tdoa' }),
    run({ method_name: 'traveltime_scan_constrained', source_position: { x_m: 1.1, y_m: 2.1, z_m: 0 } }),
  ];

  it('disabled while the scan presence is still loading (hasScan === null)', () => {
    const result = toggleAvailability(mixed, null);
    expect(result.available).toBe(false);
    expect(result.reason).toBe('checking_scan');
  });

  it('disabled with an explanation when the session has no scan', () => {
    const result = toggleAvailability(mixed, false);
    expect(result.available).toBe(false);
    expect(result.reason).toBe('no_scan');
  });

  it('disabled with an explanation when there is a scan but no constrained results yet', () => {
    const result = toggleAvailability(unconstrainedOnly, true);
    expect(result.available).toBe(false);
    expect(result.reason).toBe('no_constrained_results');
  });

  it('enabled once a scan exists and at least one constrained run is on record', () => {
    const result = toggleAvailability(mixed, true);
    expect(result.available).toBe(true);
    expect(result.reason).toBeNull();
  });

  it('a constrained run present but unsolved still counts as a constrained result on record', () => {
    const result = toggleAvailability(
      [run({ method_name: 'gauss_newton_tdoa' }), run({ method_name: 'eikonal_scan_voids', solved: false, source_position: undefined, rejection_reason: 'outside scanned envelope' })],
      true,
    );
    expect(result.available).toBe(true);
  });
});

describe('filterOverlayByVariant: toggle on/off filtering', () => {
  const runs = [
    run({ method_name: 'gauss_newton_tdoa', source_position: { x_m: 1, y_m: 1, z_m: 0 } }),
    run({ method_name: 'semblance_scan', source_position: { x_m: 1.2, y_m: 0.9, z_m: 0 }, rms_residual_us: undefined }),
    run({ method_name: 'traveltime_scan_constrained', source_position: { x_m: 1.05, y_m: 1.05, z_m: 0 } }),
    run({ method_name: 'traveltime_inversion', solved: false, source_position: undefined, rejection_reason: 'geometry is collinear' }),
    run({ method_name: 'bayesian_scan_voids', solved: false, source_position: undefined, rejection_reason: 'no scan registered' }),
  ];

  it('unconstrained view keeps only unconstrained sectors and unavailable entries', () => {
    const base = buildLocalizationOverlay(runs, geometry, null);
    const filtered = filterOverlayByVariant(base, 'unconstrained');
    expect(filtered.sectors.map((s) => s.method).sort()).toEqual(['gauss_newton_tdoa', 'semblance_scan']);
    expect(filtered.unavailable.map((u) => u.method)).toEqual(['traveltime_inversion']);
  });

  it('scan-constrained view keeps only constrained sectors and unavailable entries', () => {
    const base = buildLocalizationOverlay(runs, geometry, null);
    const filtered = filterOverlayByVariant(base, 'scan_constrained');
    expect(filtered.sectors.map((s) => s.method)).toEqual(['traveltime_scan_constrained']);
    expect(filtered.unavailable.map((u) => u.method)).toEqual(['bayesian_scan_voids']);
  });

  it('filtering never changes a surviving sector\'s coordinates or radius', () => {
    const base = buildLocalizationOverlay(runs, geometry, null);
    const before = base.sectors.find((s) => s.method === 'gauss_newton_tdoa')!;
    const after = filterOverlayByVariant(base, 'unconstrained').sectors.find((s) => s.method === 'gauss_newton_tdoa')!;
    expect(after.position).toEqual(before.position);
    expect(after.radiusM).toEqual(before.radiusM);
  });

  it('re-derives the default selection within the filtered set when the prior selection is filtered out', () => {
    const base = buildLocalizationOverlay(runs, geometry, 'traveltime_scan_constrained');
    expect(base.selectedMethod).toBe('traveltime_scan_constrained');
    const filtered = filterOverlayByVariant(base, 'unconstrained');
    /* The selected method is not in this variant, so it falls back to the first available one. */
    expect(filtered.selectedMethod).toBe('gauss_newton_tdoa');
    expect(filtered.sectors.find((s) => s.method === 'gauss_newton_tdoa')?.emphasis).toBe('selected');
  });

  it('filtering to a variant with nothing on record yields an empty (not fabricated) model', () => {
    const base = buildLocalizationOverlay([run({ method_name: 'gauss_newton_tdoa' })], geometry, null);
    const filtered = filterOverlayByVariant(base, 'scan_constrained');
    expect(filtered.sectors).toHaveLength(0);
    expect(filtered.unavailable).toHaveLength(0);
    expect(filtered.selectedMethod).toBeNull();
  });

  it('sensors are unaffected by variant filtering (session geometry, not a per-method result)', () => {
    const base = buildLocalizationOverlay(runs, geometry, null);
    const filtered = filterOverlayByVariant(base, 'scan_constrained');
    expect(filtered.sensors).toEqual(base.sensors);
  });
});

describe('unit conversion: geometry millimetres -> overlay metres', () => {
  it('converts each pod\'s mm fields to metres, one pod at a time', () => {
    const mixedUnits: LocalizationSessionGeometry = {
      available: true,
      pods: [
        { pod: 0, computed: true, leveled: true, x_mm: 1500, y_mm: -250, z_mm: 0 },
        { pod: 1, computed: true, leveled: false, x_mm: 0, y_mm: 0 }, // z_mm absent
      ],
    };
    const model = buildLocalizationOverlay([], mixedUnits, null);
    expect(model.sensors).toEqual([
      { pod: 0, label: 'MAIN', position: { x_m: 1.5, y_m: -0.25, z_m: 0 } },
      { pod: 1, label: 'Pod 1', position: { x_m: 0, y_m: 0, z_m: 0 } },
    ]);
  });

  it('excludes a pod that has no x/y millimetre fields even if computed', () => {
    const partial: LocalizationSessionGeometry = {
      available: true,
      pods: [{ pod: 0, computed: true, leveled: true }],
    };
    expect(buildLocalizationOverlay([], partial, null).sensors).toHaveLength(0);
  });

  it('excludes a pod marked not computed even with mm fields present', () => {
    const notComputed: LocalizationSessionGeometry = {
      available: true,
      pods: [{ pod: 1, computed: false, leveled: false, x_mm: 1000, y_mm: 1000, z_mm: 1000 }],
    };
    expect(buildLocalizationOverlay([], notComputed, null).sensors).toHaveLength(0);
  });
});

/* MAX-301 S5: velocity-range verdicts in the overlay. */
describe('velocity range in the overlay', () => {
  it('never picks an inconsistent method as the default primary result', () => {
    const inconsistent = run({ method_name: 'suea', computed_at_ms: 3000, velocity_status: 'inconsistent' });
    const consistent = run({ method_name: 'psafuvs', computed_at_ms: 2000, velocity_status: 'ok' });
    const model = buildLocalizationOverlay([inconsistent, consistent], geometry, null);
    expect(model.sectors.map((sector) => sector.method)).toContain('suea');
    expect(model.selectedMethod).toBe('psafuvs');
    expect(model.sectors.find((sector) => sector.method === 'suea')?.velocityInconsistent).toBe(true);
  });

  it('selects nothing by default when every method is inconsistent, but keeps them listed', () => {
    const model = buildLocalizationOverlay(
      [run({ method_name: 'suea', velocity_status: 'inconsistent' })], geometry, null,
    );
    expect(model.sectors).toHaveLength(1);
    expect(model.selectedMethod).toBeNull();
  });

  it('still lets the operator select an inconsistent method explicitly', () => {
    const model = buildLocalizationOverlay(
      [run({ method_name: 'suea', velocity_status: 'inconsistent' }), run({ method_name: 'tdcas' })],
      geometry, 'suea',
    );
    expect(model.selectedMethod).toBe('suea');
  });

  it('converts the residual to a radius at the fastest speed of the range', () => {
    const ranged = run({
      rms_residual_us: 1000,
      propagation_speed_mps: 300,
      velocity_range: { v_min_mps: 150, v_max_mps: 1500, vp_vs_min: 0, vp_vs_max: 0, source: 'preset' },
    });
    expect(sectorRadius(ranged)).toEqual({ radiusM: 1.5, source: 'residual' });
    expect(sectorRadius(run({ rms_residual_us: 1000, propagation_speed_mps: 300 })).radiusM).toBeCloseTo(0.3);
  });
});
