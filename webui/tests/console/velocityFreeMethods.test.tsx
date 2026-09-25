import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import type { LocalizationRun } from '../../src/api/deviceApi';
import { buildLocalizationOverlay, methodColor } from '../../src/domain/localizationOverlay';
import LocalizationOverlay from '../../src/features/objects/LocalizationOverlay';
import {
  estimatedVelocityText,
  sWavePickSettings,
  type SWavePickFields,
} from '../../src/features/objects/LocalizationCalculatePanel';

/*
 * Velocity-free methods in the WebUI: the optional S-wave settings PSAFUVS
 * needs (never guessed, MAX-132), the velocities TDCAS/PSAFUVS/SUEA report,
 * and their records -- scan-constrained variants included -- in the
 * comparison overlay.
 */

const COMPLETE: SWavePickFields = {
  minSeparationSamples: '20',
  maxSeparationSamples: '400',
  staWindowSamples: '10',
  ltaWindowSamples: '60',
  staLtaTriggerRatio: '3',
  minimumSnr: '2',
};

const BLANK: SWavePickFields = {
  minSeparationSamples: '', maxSeparationSamples: '', staWindowSamples: '',
  ltaWindowSamples: '', staLtaTriggerRatio: '', minimumSnr: '',
};

describe('S-wave pick settings for PSAFUVS', () => {
  it('is off when every field is blank', () => {
    expect(sWavePickSettings(BLANK)).toEqual({ state: 'off' });
    expect(sWavePickSettings({ ...BLANK, staWindowSamples: '   ' })).toEqual({ state: 'off' });
  });

  it('sends the complete, validated set with the firmware field names', () => {
    expect(sWavePickSettings(COMPLETE)).toEqual({
      state: 'on',
      payload: {
        s_min_separation_samples: 20,
        s_max_separation_samples: 400,
        s_sta_window_samples: 10,
        s_lta_window_samples: 60,
        s_sta_lta_trigger_ratio: 3,
        s_minimum_snr: 2,
      },
    });
    // A zero minimum separation is a legitimate choice.
    expect(sWavePickSettings({ ...COMPLETE, minSeparationSamples: '0' }).state).toBe('on');
  });

  it('never completes a partial set with a guess', () => {
    const partial = sWavePickSettings({ ...COMPLETE, minimumSnr: '' });
    expect(partial.state).toBe('invalid');
    expect(partial.state === 'invalid' && partial.issue).toMatch(/every field/);
  });

  it.each([
    [{ staWindowSamples: '2.5' }, /whole samples/],
    [{ staLtaTriggerRatio: '-1' }, /positive/],
    [{ maxSeparationSamples: '20' }, /maximum P-to-S separation must exceed/],
    [{ ltaWindowSamples: '10' }, /S-wave LTA window must be longer/],
    [{ staLtaTriggerRatio: '6' }, /S-wave STA\/LTA can never reach 6/],
  ] as Array<[Partial<SWavePickFields>, RegExp]>)('rejects %j', (override, message) => {
    const result = sWavePickSettings({ ...COMPLETE, ...override });
    expect(result.state).toBe('invalid');
    expect(result.state === 'invalid' ? result.issue : '').toMatch(message);
  });
});

describe('estimated velocities of velocity-free methods', () => {
  it('shows P and S for PSAFUVS, a single velocity for TDCAS/SUEA, nothing otherwise', () => {
    expect(estimatedVelocityText({ estimated_p_velocity_mps: 320.4, estimated_s_velocity_mps: 170.2 }))
      .toBe(' · estimated vP 320 m/s, vS 170 m/s');
    expect(estimatedVelocityText({ estimated_p_velocity_mps: 236.6 })).toBe(' · estimated v 237 m/s');
    expect(estimatedVelocityText({})).toBe('');
    expect(estimatedVelocityText({ estimated_p_velocity_mps: 0 })).toBe('');
  });
});

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

describe('velocity-free records in the comparison overlay', () => {
  const runs: LocalizationRun[] = [
    run('tdcas', { solved: false, source_position: undefined, rejection_reason: 'TDCAS needs 5 accepted P picks for this layout (planar surface array), have 4' }),
    run('psafuvs', { estimated_p_velocity_mps: 300, estimated_s_velocity_mps: 160 }),
    run('psafuvs_scan_constrained', { source_position: { x_m: 2.3, y_m: 1.6, z_m: -0.6 } }),
    run('suea', { source_position: { x_m: 2.4, y_m: 1.5, z_m: -1.1 }, estimated_p_velocity_mps: 280 }),
    run('suea_scan_voids', { source_position: { x_m: 2.3, y_m: 1.6, z_m: -0.84 } }),
    run('joint_fusion', { kind: 'joint_fusion' }),
  ];

  it('gives every new method its own stable colour', () => {
    const names = ['tdcas', 'tdcas_scan_constrained', 'psafuvs', 'psafuvs_scan_constrained', 'suea', 'suea_scan_voids'];
    const colors = names.map((name, index) => methodColor(name, index));
    expect(new Set(colors).size).toBe(names.length);
    expect(methodColor('suea_scan_voids', 0)).toBe(methodColor('suea_scan_voids', 7));
  });

  it('lists solved methods as sectors and keeps an unsolved TDCAS with its reason', () => {
    const overlay = buildLocalizationOverlay(runs, null, null);
    const methods = overlay.sectors.map((sector) => sector.method);
    expect(methods).toEqual(expect.arrayContaining(['psafuvs', 'psafuvs_scan_constrained', 'suea', 'suea_scan_voids']));
    expect(methods).not.toContain('tdcas');
    expect(overlay.unavailable).toEqual(expect.arrayContaining([
      expect.objectContaining({ method: 'tdcas', reason: expect.stringMatching(/have 4/) }),
    ]));
  });

  it('renders the new methods in the comparison table', () => {
    const html = renderToStaticMarkup(createElement(LocalizationOverlay, { runs, geometry: null, renderScene: false }));
    for (const name of ['psafuvs', 'suea_scan_voids', 'psafuvs_scan_constrained']) {
      expect(html).toContain(`<option value="${name}"`);
    }
    expect(html).toContain('unavailable: TDCAS needs 5 accepted P picks');
  });
});
