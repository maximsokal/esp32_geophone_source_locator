import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import LocalizationOverlay from '../../src/features/objects/LocalizationOverlay';
import type { LocalizationRun } from '../../src/api/deviceApi';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatDistance, formatNumber } from '../../src/i18n/format';

/** The rendered <input .../> tag for one radio value, regardless of React's SSR attribute order. */
function radioTag(html: string, value: string): string {
  const match = html.match(new RegExp(`<input[^>]*value="${value}"[^>]*/>`));
  if (match === null) throw new Error(`no radio input for value="${value}" in: ${html}`);
  return match[0];
}

/* MAX-161: the comparison table + selector (scene disabled: no WebGL in tests). */

beforeAll(async () => { await loadUkrainianCatalog(); });

function run(method: string, overrides: Partial<LocalizationRun> = {}): LocalizationRun {
  return {
    schema_version: 1, object_id: 'o', session_id: 's', computed_at_ms: 1,
    kind: 'individual_method', method_name: method, method_version: 1,
    channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'planar_2d',
    geometry_computed_at_ms: 1, propagation_model_name: 'constant',
    propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 10,
    confidence_score: 0.7, solved: true,
    source_position: { x_m: 1, y_m: 2, z_m: 0 }, rms_residual_us: 500,
    ...overrides,
  };
}

describe('LocalizationOverlay view', () => {
  it('shows every method, marks the selected one, and lists unavailable ones with their reason', () => {
    const html = renderToStaticMarkup(createElement(LocalizationOverlay, {
      runs: [
        run('gauss_newton_tdoa'),
        run('semblance_scan', { source_position: { x_m: -3, y_m: 1, z_m: -1 }, rms_residual_us: undefined }),
        run('traveltime_inversion', { solved: false, source_position: undefined, rejection_reason: 'fewer than 4 accepted picks' }),
      ],
      geometry: null,
      renderScene: false,
    }));
    expect(html).toContain('<option value="gauss_newton_tdoa" selected="">gauss_newton_tdoa</option>');
    expect(html).toContain('<option value="semblance_scan">semblance_scan</option>');
    /* The failed method is never an option and never a sector. */
    expect(html).not.toContain('<option value="traveltime_inversion"');
    expect(html).toContain(translate('en', 'overlay.unavailable_reason', { reason: 'fewer than 4 accepted picks' }));
    expect(html.match(/<tr class="selected">/g)?.length).toBe(1);
    expect(html.match(/<tr class="dim">/g)?.length).toBe(1);
    expect(html).toContain(formatDistance('en', 0.15)); /* 500 us x 300 m/s */
    expect(html).toContain(`(${translate('en', 'overlay.nominal')})`);
    expect(html).toContain(translate('en', 'overlay.note'));
  });

  it('renders nothing without runs', () => {
    expect(renderToStaticMarkup(createElement(LocalizationOverlay, { runs: [], geometry: null, renderScene: false }))).toBe('');
  });

  it.each(['en', 'uk'] as const)('renders the localized overlay heading and scan explanation in %s', (locale: I18nLocale) => {
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(LocalizationOverlay, {
      runs: [run('gauss_newton_tdoa')], geometry: null, hasScan: false, renderScene: false,
    })));
    expect(html).toContain(`aria-label="${translate(locale, 'overlay.aria_label')}"`);
    expect(html).toContain(`<h3>${translate(locale, 'overlay.title')}</h3>`);
    expect(html).toContain(translate(locale, 'overlay.no_scan'));
    expect(/(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/.test(html.replace(/<[^>]*>/g, ' '))).toBe(false);
  });

  it.each(['en', 'uk'] as const)('keeps missing coordinates unavailable and uncertainty unknown in %s', (locale: I18nLocale) => {
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(LocalizationOverlay, {
      runs: [
        run('gauss_newton_tdoa', { uncertainty: undefined }),
        run('traveltime_inversion', {
          solved: false, source_position: undefined, rms_residual_us: undefined, rejection_reason: 'NO_SOURCE_POSITION',
        }),
      ],
      geometry: null,
      renderScene: false,
    })));
    const selectedRow = html.match(/<tr class="selected">[\s\S]*?<\/tr>/)?.[0];
    const unavailableRow = html.match(/<tr class="unavailable">[\s\S]*?<\/tr>/)?.[0];
    expect(selectedRow).toBeDefined();
    expect(selectedRow).toContain('<td>\u2014</td><td>2D</td>');
    expect(unavailableRow).toContain('NO_SOURCE_POSITION');
    const localizedZero = formatNumber(locale, 0, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
    expect(unavailableRow).not.toContain(`${localizedZero}, ${localizedZero}, ${localizedZero}`);
  });
});

/* MAX-148: the unconstrained/scan-constrained toggle. */
describe('LocalizationOverlay unconstrained/scan-constrained toggle', () => {
  it('is disabled with an explanation when the session has no scan', () => {
    const html = renderToStaticMarkup(createElement(LocalizationOverlay, {
      runs: [run('gauss_newton_tdoa')],
      geometry: null,
      hasScan: false,
      renderScene: false,
    }));
    expect(html).toContain('<fieldset class="overlay-variant-toggle" disabled="">');
    expect(radioTag(html, 'unconstrained')).toContain('disabled=""');
    expect(radioTag(html, 'unconstrained')).toContain('checked=""');
    expect(radioTag(html, 'scan_constrained')).toContain('disabled=""');
    expect(html).toContain('This session has no 3D scan');
  });

  it('is disabled with a different explanation when a scan exists but no constrained results have been computed', () => {
    const html = renderToStaticMarkup(createElement(LocalizationOverlay, {
      runs: [run('gauss_newton_tdoa')],
      geometry: null,
      hasScan: true,
      renderScene: false,
    }));
    expect(html).toContain('<fieldset class="overlay-variant-toggle" disabled="">');
    expect(html).toContain('No scan-constrained localization results');
  });

  it('is disabled with a "checking" explanation while scan presence is unknown (hasScan omitted)', () => {
    const html = renderToStaticMarkup(createElement(LocalizationOverlay, {
      runs: [run('gauss_newton_tdoa')],
      geometry: null,
      renderScene: false,
    }));
    expect(html).toContain('<fieldset class="overlay-variant-toggle" disabled="">');
    expect(html).toContain('Checking whether this session has a 3D scan');
  });

  it('is enabled once a scan exists and a scan-constrained result is on record, and the default view filters the table to unconstrained', () => {
    const html = renderToStaticMarkup(createElement(LocalizationOverlay, {
      runs: [
        run('gauss_newton_tdoa', { source_position: { x_m: 1, y_m: 1, z_m: 0 } }),
        run('traveltime_scan_constrained', { source_position: { x_m: 1.1, y_m: 1.1, z_m: 0 } }),
      ],
      geometry: null,
      hasScan: true,
      renderScene: false,
    }));
    expect(html).toContain('<fieldset class="overlay-variant-toggle">');
    expect(html).not.toContain('<fieldset class="overlay-variant-toggle" disabled="">');
    expect(radioTag(html, 'unconstrained')).toContain('checked=""');
    expect(radioTag(html, 'unconstrained')).not.toContain('disabled=""');
    expect(radioTag(html, 'scan_constrained')).not.toContain('checked=""');
    expect(radioTag(html, 'scan_constrained')).not.toContain('disabled=""');
    /* Default view is unconstrained: only the unconstrained method appears in the table/selector. */
    expect(html).toContain('>gauss_newton_tdoa<');
    expect(html).not.toContain('traveltime_scan_constrained');
  });
});
