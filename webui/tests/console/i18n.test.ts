import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { readFileSync } from 'node:fs';

import { LocaleProvider, persistLocale, readSavedLocale, translate, useLocale, type I18nKey, type I18nLocale } from '../../src/i18n';
import { en, uk } from '../../src/i18n/catalog.gen';
import { formatDate, formatDistance, formatNumber, formatPercent } from '../../src/i18n/format';
import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';

type CatalogEntry = { en: string; uk: string };
type CatalogFile = { entries: Record<string, CatalogEntry> };
const readCatalog = (url: URL): CatalogFile => JSON.parse(readFileSync(url, 'utf8')) as CatalogFile;
const hasLeakedKey = (html: string): boolean => /(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/.test(html.replace(/<[^>]*>/g, ' '));

afterEach(() => vi.unstubAllGlobals());

describe('i18n catalog translation', () => {
  it('falls back to English when the Ukrainian catalog entry is empty', () => {
    expect(translate('uk', 'app.title')).toBe('GeoLocator 3D');
  });

  it('substitutes provided parameters and leaves missing ones visible', () => {
    expect(translate('en', 'sensors.power_fault', { voltage: 4.2 })).toContain('4.2 V');
    expect(translate('en', 'sensors.power_fault')).toContain('{voltage}');
  });

  it('returns an unknown runtime key unchanged', () => {
    expect(translate('en', 'missing.key' as I18nKey)).toBe('missing.key');
  });

  it('keeps WebUI and report catalogs bilingual and current in the generated catalog', () => {
    for (const url of [new URL('../../../i18n/web.json', import.meta.url), new URL('../../../i18n/report.json', import.meta.url)]) {
      const entries = readCatalog(url).entries;
      for (const [key, value] of Object.entries(entries)) {
        expect(value.en.trim(), `${key} English`).not.toBe('');
        expect(value.uk.trim(), `${key} Ukrainian`).not.toBe('');
        expect(en[key as I18nKey]).toBe(value.en);
        expect(uk[key as I18nKey]).toBe(value.uk);
      }
    }
    expect(Object.keys(en).sort()).toEqual(Object.keys(uk).sort());
  });

  it.each(['en', 'uk'] as const)('renders the console header and objects screen in %s without leaking keys', (locale: I18nLocale) => {
    const header = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(DeviceConsoleApp)));
    expect(header).toContain(`aria-label="${translate(locale, 'web.locale_label')}"`);
    expect(hasLeakedKey(header)).toBe(false);

    const noop = async () => undefined;
    const objects = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(ObjectsFeature, {
      status: null, store: null, sessions: null, selectedId: null, busy: false, error: null,
      onSelect: () => undefined, onCreate: noop, onActivate: noop, onRename: noop, onDelete: noop,
      onBindGps: async () => ({ latitude: 0, longitude: 0, accuracy_m: 0 }),
      onCable: noop, onDefaultCable: noop, onAcquisition: noop, onRecording: noop, onOpenScanner: () => undefined,
    })));
    expect(objects).toContain(translate(locale, 'objects.database'));
    expect(objects).toContain(translate(locale, 'objects.create'));
    expect(hasLeakedKey(objects)).toBe(false);
  });

  it('uses English for an unknown runtime locale', () => {
    expect(translate('xx' as I18nLocale, 'nav.objects')).toBe('Objects');
  });

  it('formats numbers, percentages, distances, and dates with an explicit app locale', () => {
    const date = new Date('2026-09-26T12:34:00.000Z');
    expect(formatNumber('en', 1234)).toBe(new Intl.NumberFormat('en-US').format(1234));
    expect(formatNumber('uk', 1234)).toBe(new Intl.NumberFormat('uk-UA').format(1234));
    expect(formatPercent('uk', 0.25)).toBe(new Intl.NumberFormat('uk-UA', { style: 'percent', maximumFractionDigits: 0 }).format(0.25));
    expect(formatDistance('en', 1.25)).toBe(`${new Intl.NumberFormat('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(1.25)} m`);
    expect(formatDate('uk', date)).toBe(new Intl.DateTimeFormat('uk-UA', { dateStyle: 'medium', timeStyle: 'short' }).format(date));
  });

  it('is independent of the host Intl default locale', () => {
    const originalNumberFormat = Intl.NumberFormat;
    const originalDateTimeFormat = Intl.DateTimeFormat;
    const inHostLocale = (hostLocale: string) => {
      Object.defineProperty(Intl, 'NumberFormat', {
        configurable: true,
        value: class extends originalNumberFormat {
          constructor(locales?: Intl.LocalesArgument, options?: Intl.NumberFormatOptions) {
            super(locales ?? hostLocale, options);
          }
        },
      });
      Object.defineProperty(Intl, 'DateTimeFormat', {
        configurable: true,
        value: class extends originalDateTimeFormat {
          constructor(locales?: Intl.LocalesArgument, options?: Intl.DateTimeFormatOptions) {
            super(locales ?? hostLocale, options);
          }
        },
      });
      return ['en', 'uk'].map((locale) => [
        formatNumber(locale as I18nLocale, 1234567.89),
        formatPercent(locale as I18nLocale, 0.1234, 1),
        formatDistance(locale as I18nLocale, 1234.5),
        formatDate(locale as I18nLocale, new Date('2026-09-26T12:34:00.000Z')),
      ]);
    };
    try {
      expect(inHostLocale('en-US')).toEqual(inHostLocale('de-DE'));
    } finally {
      Object.defineProperty(Intl, 'NumberFormat', { configurable: true, value: originalNumberFormat });
      Object.defineProperty(Intl, 'DateTimeFormat', { configurable: true, value: originalDateTimeFormat });
    }
  });

  it('defaults to English for missing, malformed, unknown, stale, and denied storage', () => {
    const values: Record<string, string | null> = { 'geolocator.locale': null };
    vi.stubGlobal('window', { localStorage: {
      getItem: (key: string) => values[key] ?? null,
      setItem: (key: string, value: string) => { values[key] = value; },
    } });
    expect(readSavedLocale()).toBe('en');
    values['geolocator.locale'] = '{';
    expect(readSavedLocale()).toBe('en');
    values['geolocator.locale'] = JSON.stringify({ schema: 1, locale: '' });
    expect(readSavedLocale()).toBe('en');
    values['geolocator.locale'] = JSON.stringify({ schema: 1, locale: 'xx' });
    expect(readSavedLocale()).toBe('en');
    values['geolocator.locale'] = JSON.stringify({ schema: 0, locale: 'uk' });
    expect(readSavedLocale()).toBe('en');
    values['geolocator.locale'] = JSON.stringify({ schema: 1, locale: 'uk' });
    expect(readSavedLocale()).toBe('uk');
    vi.stubGlobal('window', { get localStorage(): never { throw new Error('storage denied'); } });
    expect(readSavedLocale()).toBe('en');
    expect(() => persistLocale('uk')).not.toThrow();
  });

  it('persists the locale under a versioned schema and uses English when storage is denied', () => {
    const setItem = vi.fn();
    vi.stubGlobal('window', { localStorage: { setItem } });
    persistLocale('uk');
    expect(setItem).toHaveBeenCalledWith('geolocator.locale', JSON.stringify({ schema: 1, locale: 'uk' }));
    vi.stubGlobal('window', { get localStorage(): never { throw new Error('storage denied'); } });
    expect(() => persistLocale('en')).not.toThrow();
  });

  it('uses the explicitly supplied initial locale in the provider', () => {
    function Probe() {
      const { locale } = useLocale();
      return createElement('span', null, locale);
    }
    expect(renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'uk' }, createElement(Probe)))).toContain('uk');
  });
});
