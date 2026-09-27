import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import type { LocalizationRun } from '../../src/api/deviceApi';
import { LocaleProvider, translate } from '../../src/i18n';
import { formatNumber, formatPercent } from '../../src/i18n/format';
import {
  buildLocalizationOverlay,
  credibleBox,
  methodColor,
  sectorRadius,
  uncertaintyText,
} from '../../src/domain/localizationOverlay';
import LocalizationOverlay from '../../src/features/objects/LocalizationOverlay';
import { estimatedVelocityText } from '../../src/features/objects/LocalizationCalculatePanel';

/*
 * The extended localization methods in the WebUI: cross-correlation TDOA,
 * robust fit, PSP, eikonal (with scan voids), eikonal-focused semblance and
 * the Bayesian posterior -- their colours, and the posterior region the
 * Bayesian records carry (table text, sector radius, 3D box).
 */

function run(method: string, overrides: Partial<LocalizationRun> = {}): LocalizationRun {
  return {
    schema_version: 3, object_id: 'OBJ_0001', session_id: 'S000001', computed_at_ms: 10,
    kind: 'individual_method', method_name: method, method_version: 1,
    channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'capable_3d',
    geometry_computed_at_ms: 1, propagation_model_name: 'tap', propagation_speed_mps: 300,
    propagation_speed_uncertainty_mps: 10, confidence_score: 0.8, solved: true,
    source_position: { x_m: 2.3, y_m: 1.6, z_m: -0.6 }, rms_residual_us: 20,
    ...overrides,
  };
}

/* Exactly the object the firmware's run-record JSON writer emits
 * (components/localization_results: "uncertainty" is written only for a
 * solved run with a credible level). */
const FIRMWARE_JSON = '{"schema_version":3,"object_id":"OBJ_0001","session_id":"S000001","computed_at_ms":10,' +
  '"kind":"individual_method","method_name":"bayesian_scan_voids","method_version":1,"channel_mask_used":15,' +
  '"channel_mask_rejected":0,"dimensionality":"capable_3d","geometry_computed_at_ms":1,' +
  '"propagation_model_name":"tap","propagation_speed_mps":300,"propagation_speed_uncertainty_mps":10,' +
  '"confidence_score":0.9,"degrees_of_freedom":3,"solved":true,"source_position":{"x_m":2.3,"y_m":1.6,"z_m":-0.6},' +
  '"emission_time_us":1000000,"rms_residual_us":6,"iterations":0,"converged":false,"estimated_p_velocity_mps":301.5,' +
  '"uncertainty":{"std_m":{"x_m":0.03,"y_m":0.01,"z_m":0.05},"credible_level":0.9,"credible_volume_m3":0.0008,' +
  '"credible_area_m2":0,"credible_min":{"x_m":2.25,"y_m":1.575,"z_m":-0.7},"credible_max":{"x_m":2.35,"y_m":1.625,"z_m":-0.5}}}';

const NEW_METHODS = [
  'xcorr_tdoa', 'robust_tdoa', 'psp_joint', 'eikonal_fmm', 'eikonal_scan_voids',
  'semblance_eikonal_voids', 'bayesian', 'bayesian_scan_voids',
];
const EXISTING_METHODS = [
  'energy_baseline', 'gauss_newton_tdoa', 'traveltime_inversion', 'semblance_scan', 'traveltime_scan_constrained',
  'semblance_scan_constrained', 'tdcas', 'tdcas_scan_constrained', 'psafuvs', 'psafuvs_scan_constrained', 'suea',
  'suea_scan_voids', 'joint_fusion',
];

describe('colours of the extended methods', () => {
  it('gives each its own stable colour, distinct from every existing method', () => {
    const colors = [...NEW_METHODS, ...EXISTING_METHODS].map((name, index) => methodColor(name, index));
    expect(new Set(colors).size).toBe(colors.length);
    for (const name of NEW_METHODS) expect(methodColor(name, 0)).toBe(methodColor(name, 5));
  });
});

describe('posterior region of a Bayesian run', () => {
  const bayes: LocalizationRun = JSON.parse(FIRMWARE_JSON) as LocalizationRun;
  const fixed = { minimumFractionDigits: 2, maximumFractionDigits: 2 };
  const sd = [0.03, 0.01, 0.05].map((value) => formatNumber('en', value, fixed)).join(' / ');
  const region = translate('en', 'overlay.region_percent', { percent: formatPercent('en', 0.9) });
  const uncertaintyEn = `±${sd} m (${translate('en', 'overlay.one_sd')}) · ${region} ${formatNumber('en', 0.0008, { notation: 'scientific', maximumFractionDigits: 1 })} m³`;

  it('reads the firmware field names', () => {
    expect(bayes.uncertainty?.credible_level).toBe(0.9);
    expect(bayes.uncertainty?.std_m.z_m).toBe(0.05);
    expect(estimatedVelocityText(bayes)).toBe(' · estimated v 302 m/s');
  });

  it('formats the per-axis sd and the region size (volume in 3D, area when planar)', () => {
    expect(uncertaintyText(bayes, 'en')).toBe(uncertaintyEn);
    const planar = { ...bayes.uncertainty!, credible_volume_m3: 0, credible_area_m2: 0.25 };
    expect(uncertaintyText({ uncertainty: planar }, 'en')).toBe(
      `±${sd} m (${translate('en', 'overlay.one_sd')}) · ${region} ${formatNumber('en', 0.25, fixed)} m²`,
    );
    expect(uncertaintyText({}, 'en')).toBe('');
    expect(uncertaintyText({ uncertainty: { ...bayes.uncertainty!, credible_level: 0 } }, 'en')).toBe('');
  });

  it('turns the region into a box and the sector radius into 2 sd', () => {
    const box = credibleBox(bayes)!;
    expect(box.level).toBe(0.9);
    expect(box.center.x_m).toBeCloseTo(2.3, 12);
    expect(box.center.z_m).toBeCloseTo(-0.6, 12);
    expect(box.size.x_m).toBeCloseTo(0.1, 12);
    expect(box.size.z_m).toBeCloseTo(0.2, 12);
    expect(sectorRadius(bayes)).toEqual({ radiusM: 0.1, source: 'posterior' });
    // Without a posterior the residual still sets the radius.
    expect(credibleBox(run('xcorr_tdoa'))).toBeNull();
    const slow = sectorRadius(run('xcorr_tdoa', { rms_residual_us: 400 }));
    expect(slow.source).toBe('residual');
    expect(slow.radiusM).toBeCloseTo(400e-6 * 300, 12);
    // A malformed region is ignored rather than drawn.
    const broken = { ...bayes, uncertainty: { ...bayes.uncertainty!, credible_min: { x_m: Number.NaN, y_m: 0, z_m: 0 } } };
    expect(credibleBox(broken)).toBeNull();
  });
});

describe('extended methods in the comparison overlay', () => {
  const bayes: LocalizationRun = JSON.parse(FIRMWARE_JSON) as LocalizationRun;
  const runs: LocalizationRun[] = [
    run('xcorr_tdoa', { source_position: { x_m: 2.31, y_m: 1.61, z_m: -0.6 } }),
    run('robust_tdoa'),
    run('psp_joint', {
      solved: false, source_position: undefined,
      rejection_reason: 'underdetermined: depth trades against vP (set a vP uncertainty or add an off-plane pod)',
    }),
    run('eikonal_scan_voids', { estimated_p_velocity_mps: 296 }),
    run('semblance_eikonal_voids', { rms_residual_us: undefined }),
    bayes,
  ];

  it('places solved methods as sectors, with the Bayesian region as a box', () => {
    const overlay = buildLocalizationOverlay(runs, null, 'bayesian_scan_voids');
    const byMethod = new Map(overlay.sectors.map((sector) => [sector.method, sector]));
    expect([...byMethod.keys()].sort()).toEqual(
      ['bayesian_scan_voids', 'eikonal_scan_voids', 'robust_tdoa', 'semblance_eikonal_voids', 'xcorr_tdoa'],
    );
    expect(byMethod.get('bayesian_scan_voids')!.radiusSource).toBe('posterior');
    expect(byMethod.get('bayesian_scan_voids')!.credibleBox).not.toBeNull();
    expect(byMethod.get('bayesian_scan_voids')!.emphasis).toBe('selected');
    expect(byMethod.get('semblance_eikonal_voids')!.radiusSource).toBe('nominal');
    expect(byMethod.get('xcorr_tdoa')!.credibleBox).toBeNull();
    expect(overlay.unavailable).toEqual([
      expect.objectContaining({ method: 'psp_joint', reason: expect.stringMatching(/^underdetermined/) }),
    ]);
  });

  it('shows the uncertainty column and keeps the unavailable reason across the table', () => {
    const sd = [0.03, 0.01, 0.05].map((value) => formatNumber('en', value, {
      minimumFractionDigits: 2, maximumFractionDigits: 2,
    })).join(' / ');
    const uncertaintyEn = `±${sd} m (${translate('en', 'overlay.one_sd')}) · ${translate('en', 'overlay.region_percent', {
      percent: formatPercent('en', 0.9),
    })} ${formatNumber('en', 0.0008, { notation: 'scientific', maximumFractionDigits: 1 })} m³`;
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'en' },
      createElement(LocalizationOverlay, { runs, geometry: null, renderScene: false })));
    expect(html).toContain(`<th>${translate('en', 'overlay.uncertainty')}</th>`);
    expect(html).toContain(uncertaintyEn);
    expect(html).toContain(`${formatNumber('en', 0.1, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m (${translate('en', 'overlay.two_sd')})`);
    expect(html).toContain(`<td colSpan="6">${translate('en', 'overlay.unavailable_reason', {
      reason: 'underdetermined: depth trades against vP (set a vP uncertainty or add an off-plane pod)',
    })}</td>`);
    for (const name of NEW_METHODS.filter((name) => name !== 'psp_joint' && name !== 'eikonal_fmm' && name !== 'bayesian')) {
      expect(html).toContain(`<option value="${name}"`);
    }
  });
});
