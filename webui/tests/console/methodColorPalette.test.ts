import { describe, expect, it } from 'vitest';
import { methodColor } from '../../src/domain/localizationOverlay';
import { mixHex, uiTheme } from '../../src/theme';
import overlaySource from '../../src/domain/localizationOverlay.ts?raw';

/* S12: localization method colours are built from the 11 base roles only. */
const METHODS = [
  'energy_baseline', 'gauss_newton_tdoa', 'robust_tdoa', 'xcorr_tdoa', 'traveltime_inversion',
  'traveltime_scan_constrained', 'tdcas', 'tdcas_scan_constrained', 'semblance_scan',
  'semblance_scan_constrained', 'semblance_eikonal_voids', 'psafuvs', 'psafuvs_scan_constrained',
  'psp_joint', 'suea', 'suea_scan_voids', 'eikonal_fmm', 'eikonal_scan_voids', 'bayesian',
  'bayesian_scan_voids', 'joint_fusion',
];

function luminance(hex: string): number {
  const [r, g, b] = [1, 3, 5].map((i) => {
    const v = parseInt(hex.slice(i, i + 2), 16) / 255;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
const contrast = (a: string, b: string) => {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
};

describe('localization method colours', () => {
  it('gives every known method and fallback its own colour', () => {
    const colours = [...METHODS.map((m, i) => methodColor(m, i)), ...[0, 1, 2, 3].map((i) => methodColor('unknown_method', i))];
    expect(new Set(colours).size).toBe(colours.length);
  });

  it('keeps hue roles recognisable (fusion = TEXT, baseline = MUTED, TDOA base = LINK)', () => {
    expect(methodColor('joint_fusion', 0)).toBe(uiTheme.text);
    expect(methodColor('energy_baseline', 0)).toBe(uiTheme.muted);
    expect(methodColor('gauss_newton_tdoa', 0)).toBe(uiTheme.linkMeasuring);
    expect(methodColor('eikonal_scan_voids', 0)).toBe(mixHex(uiTheme.error, uiTheme.text, 0.35));
  });

  it('stays readable on the console panel (non-text contrast >= 3)', () => {
    for (const m of METHODS) expect(contrast(methodColor(m, 0), uiTheme.panel), m).toBeGreaterThanOrEqual(3);
  });

  it('contains no private hex colours', () => {
    expect(overlaySource.match(/#[0-9a-fA-F]{6}\b/g) ?? []).toEqual([]);
  });
});
