import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import type { DeviceStatus } from '../../src/domain/device';
import StorageUnavailableBanner from '../../src/components/StorageUnavailableBanner';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';

function render(status: DeviceStatus | null, locale: I18nLocale = 'en'): string {
  return renderToStaticMarkup(createElement(
    LocaleProvider,
    { initialLocale: locale },
    createElement(StorageUnavailableBanner, { status }),
  ));
}

beforeAll(async () => { await loadUkrainianCatalog(); });

describe('storage unavailable banner', () => {
  it.each(['en', 'uk'] as const)('shows the translated message in %s', (locale) => {
    const html = render({ storage_available: false } as DeviceStatus, locale);
    expect(html).toContain(translate(locale, 'web.storage.unavailable_banner'));
    expect(html).toContain('role="status"');
  });

  it('treats a missing availability field as available for older firmware', () => {
    expect(render({} as DeviceStatus)).toBe('');
    expect(render({ storage_available: true } as DeviceStatus)).toBe('');
    expect(render(null)).toBe('');
  });
});
