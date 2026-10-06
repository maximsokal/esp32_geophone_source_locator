import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import HardwareInventoryPanel from '../../src/features/dashboard/HardwareInventoryPanel';
import type { DeviceStatus } from '../../src/domain/device';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nKey, type I18nLocale } from '../../src/i18n';

function render(status: DeviceStatus | null, locale: I18nLocale = 'en'): string {
  return renderToStaticMarkup(createElement(
    LocaleProvider,
    { initialLocale: locale },
    createElement(HardwareInventoryPanel, { status }),
  ));
}

beforeAll(async () => { await loadUkrainianCatalog(); });

describe('hardware inventory dashboard panel', () => {
  it.each(['en', 'uk'] as const)('shows module state, requirement and reason in %s', (locale) => {
    const status = {
      hardware: {
        modules: [
          { id: 'sd_card', state: 'present', required: true, reason: 'mounted' },
          { id: 'barometer', state: 'degraded', required: false, reason: 'slow response' },
        ],
        required_missing: true,
      },
    } as unknown as DeviceStatus;

    const html = render(status, locale);
    expect(html).toContain(translate(locale, 'web.hw.title' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.module.sd_card' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.state.present' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.required' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.optional' as I18nKey));
    expect(html).toContain('mounted');
    expect(html).toContain('slow response');
    expect(html).toContain(translate(locale, 'web.hw.degraded_banner' as I18nKey));
  });

  it('falls back to raw identifiers for unknown modules and states', () => {
    const status = {
      hardware: {
        modules: [{ id: 'custom_board', state: 'awaiting_probe', required: false, reason: '' }],
        required_missing: false,
      },
    } as unknown as DeviceStatus;

    const html = render(status);
    expect(html).toContain('custom_board');
    expect(html).toContain('awaiting_probe');
    expect(html).toContain('hardware-state-unknown');
  });

  it('does not show the panel when older firmware omits hardware', () => {
    const html = render({} as DeviceStatus);
    expect(html).not.toContain('hardware-inventory-card');
  });
});
