import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import LevelingCard from '../../src/features/sensors/LevelingCard';
import type { DevicePod, RecordingGateStatus } from '../../src/domain/device';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';

/* MAX-114: which geophone blocks recording, and by how much. */

describe('LevelingCard', () => {
  it.each(['en', 'uk'] as const)('lists each node with localized state and tilt in %s', async (locale: I18nLocale) => {
    if (locale === 'uk') await loadUkrainianCatalog();
    const gate: RecordingGateStatus = {
      available: true,
      assigned_count: 3,
      accepted_count: 1,
      allowed: false,
      decision: 'BLOCKED_NOT_LEVEL',
      reason: 'Level every connected geophone before recording.',
      nodes: [
        { channel: 0, node_id: 1, state: 'level', tilt_deg: 0.4 },
        { channel: 2, node_id: 3, state: 'tilted', tilt_deg: 12.34 },
        { channel: 5, node_id: 6, state: 'no_data' },
      ],
    };
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(LevelingCard, { gate, pods: [], onCalibrateHeights: async () => true })));
    expect(html).toContain(translate(locale, 'leveling.count_level', { accepted: formatNumber(locale, 1), assigned: formatNumber(locale, 3) }));
    expect(html).toContain(translate(locale, 'recording_gate.reason.not_level'));
    expect(html).toContain(translate(locale, 'leveling.tilted'));
    expect(html).toContain(`${formatNumber(locale, 12.34, { maximumFractionDigits: 1 })}°`);
    expect(html).toContain(translate(locale, 'leveling.no_data'));
    expect(html).toContain(translate(locale, 'leveling.calibrate_heights'));
    expect(/(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/.test(html.replace(/<[^>]*>/g, ' '))).toBe(false);
    expect(html.match(/class="blocking"/g)?.length).toBe(2);
    expect(html.match(/class="ok"/g)?.length).toBe(1);
    /* Channels are shown 1-based like the pod cards. */
    expect(html).toContain('<td>6</td>');
  });

  it.each(['en', 'uk'] as const)('renders calibrated height labels in %s', async (locale: I18nLocale) => {
    if (locale === 'uk') await loadUkrainianCatalog();
    const pods = [{ id: 1, state: 'CONNECTED', height_reference: { state: 'valid', generation: 2, relative_z_mm: 12.34 } }] as DevicePod[];
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(LevelingCard, {
      gate: undefined,
      pods,
      main: { height_reference: { state: 'rejected_unstable', generation: 2, relative_z_mm: 0 } },
      onCalibrateHeights: async () => true,
    })));
    expect(html).toContain(translate(locale, 'leveling.rejected_pressure_unstable'));
    expect(html).toContain(translate(locale, 'leveling.valid'));
    expect(html).toContain(`${formatNumber(locale, 12.34, { maximumFractionDigits: 1 })} mm`);
  });

  it('explains an unavailable topology and keeps calibration available without gate data', () => {
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(LevelingCard, {
      gate: { available: false, error: 'ESP_ERR_INVALID_STATE' },
      pods: [],
      onCalibrateHeights: async () => true,
    })));
    expect(html).toContain(translate('en', 'leveling.unavailable'));
    expect(html).toContain('ESP_ERR_INVALID_STATE');
    expect(renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(LevelingCard, {
      gate: undefined,
      pods: [],
      onCalibrateHeights: async () => true,
    })))).toContain(translate('en', 'leveling.calibrate_heights'));
  });
});
