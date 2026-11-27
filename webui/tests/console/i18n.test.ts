import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { readFileSync } from 'node:fs';

import { formatErrorText, loadUkrainianCatalog, LocaleProvider, persistLocale, readSavedLocale, TranslatedError, translate, useLocale, useT, type I18nKey, type I18nLocale } from '../../src/i18n';
import { en } from '../../src/i18n/catalog.en.gen';
import { uk } from '../../src/i18n/catalog.uk.gen';
import { formatDate, formatDistance, formatNumber, formatPercent } from '../../src/i18n/format';
import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';

type CatalogEntry = { en: string; uk: string };
type CatalogFile = { entries: Record<string, CatalogEntry> };
const readCatalog = (url: URL): CatalogFile => JSON.parse(readFileSync(url, 'utf8')) as CatalogFile;
const hasLeakedKey = (html: string): boolean => /(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/.test(html.replace(/<[^>]*>/g, ' '));

afterEach(() => vi.unstubAllGlobals());
beforeAll(async () => { await loadUkrainianCatalog(); });

describe('i18n catalog translation', () => {
  it('keeps the product brand explicitly identical in both locales', () => {
    expect(translate('uk', 'app.title')).toBe('GeoLocator 3D');
    expect(en['app.title']).toBe(uk['app.title']);
  });

  it.each(['en', 'uk'] as const)('formats unknown error detail with the localized prefix in %s', (locale) => {
    const detail = 'ESP_ERR_TRANSPORT_RESET';
    expect(formatErrorText(locale, detail)).toBe(translate(locale, 'web.error_prefix', { detail }));
  });

  it.each(['en', 'uk'] as const)('maps raw text that matches an English catalog value in %s', (locale) => {
    expect(formatErrorText(locale, en['scan.storage.paused'])).toBe(translate(locale, 'scan.storage.paused'));
  });

  it('passes explicitly translated errors through unchanged', () => {
    const translated = new TranslatedError('Помилка: already translated');
    expect(formatErrorText('en', translated)).toBe(translated.message);
    const keyed = TranslatedError.fromKey('console.error.object_store_unavailable', {
      detail: TranslatedError.fromKey('objects.storage_error'),
    });
    for (const locale of ['en', 'uk'] as const) {
      expect(formatErrorText(locale, keyed)).toBe(translate(locale, 'console.error.object_store_unavailable', {
        detail: translate(locale, 'objects.storage_error'),
      }));
    }
  });

  it.each(['en', 'uk'] as const)('does not prefix a formatted error again when marked in %s', (locale) => {
    const formatted = formatErrorText(locale, 'ESP_ERR_TRANSPORT_RESET');
    expect(formatErrorText(locale, new TranslatedError(formatted))).toBe(formatted);
  });

  it('renders the migrated saved-scan warning in both locales', () => {
    expect(translate('en', 'scan.error.continue_unavailable')).toBe(
      'No durable saved 3D scan is available to continue. Open or finish a scan first.',
    );
    expect(translate('uk', 'scan.error.continue_unavailable')).toBe(
      'Немає збереженого 3D-скану для продовження. Спершу відкрийте або завершіть сканування.',
    );
  });

  it('substitutes provided parameters and leaves missing ones visible', () => {
    expect(translate('en', 'sensors.power_fault', { voltage: 4.2 })).toContain('4.2 V');
    expect(translate('en', 'sensors.power_fault')).toContain('{voltage}');
  });

  it('returns an unknown runtime key unchanged', () => {
    expect(translate('en', 'missing.key' as I18nKey)).toBe('missing.key');
  });

  it('uses deterministic English fallback for fixed-seed unknown locale ids and keys', () => {
    let random = 0x189C0FFE;
    const nextRandom = (): number => {
      random = (Math.imul(random, 1664525) + 1013904223) >>> 0;
      return random;
    };
    for (let sample = 0; sample < 512; sample += 1) {
      const key = `__missing.${nextRandom().toString(16)}.${sample}` as I18nKey;
      const locale = `unknown-${nextRandom().toString(16)}` as I18nLocale;
      expect(translate(locale, key)).toBe(key);
    }
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
      onSelect: () => undefined, onCreate: async () => true, onActivate: noop, onRename: noop, onDelete: noop,
      onBindGps: async () => ({ latitude: 0, longitude: 0, accuracy_m: 0 }),
      onCable: noop, onDefaultCable: noop, onAcquisition: noop, onRecording: noop, onOpenScanner: () => undefined,
    })));
    expect(objects).toContain(translate(locale, 'nav.objects'));
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
    const values: Record<string, string | null> = { 'geolocator.locale': null, 'geolocator.theme': 'dark' };
    vi.stubGlobal('window', { localStorage: {
      getItem: (key: string) => values[key] ?? null,
      setItem: (key: string, value: string) => { values[key] = value; },
    } });
    expect(readSavedLocale()).toBe('en');
    values['geolocator.locale'] = '';
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
    expect(values['geolocator.theme']).toBe('dark');
    vi.stubGlobal('window', { get localStorage(): never { throw new Error('storage denied'); } });
    expect(readSavedLocale()).toBe('en');
    expect(() => persistLocale('uk')).not.toThrow();
  });

  it('persists the locale under a versioned schema and uses English when storage is denied', () => {
    const values: Record<string, string> = { 'geolocator.theme': 'dark' };
    const setItem = vi.fn((key: string, value: string) => { values[key] = value; });
    vi.stubGlobal('window', { localStorage: { setItem } });
    persistLocale('uk');
    expect(setItem).toHaveBeenCalledWith('geolocator.locale', JSON.stringify({ schema: 1, locale: 'uk' }));
    expect(values['geolocator.theme']).toBe('dark');
    vi.stubGlobal('window', { get localStorage(): never { throw new Error('storage denied'); } });
    expect(() => persistLocale('en')).not.toThrow();
  });

  it('renders in English when the browser denies access to localStorage', () => {
    vi.stubGlobal('window', { get localStorage(): never { throw new Error('storage denied'); } });
    function LocaleProbe() {
      const { locale } = useLocale();
      return createElement('span', null, `${locale}:${useT()('nav.objects')}`);
    }
    const html = renderToStaticMarkup(createElement(LocaleProvider, null, createElement(LocaleProbe)));
    expect(html).toContain('<span>en:Objects</span>');
  });

  it('uses the explicitly supplied initial locale in the provider', () => {
    function Probe() {
      const { locale } = useLocale();
      return createElement('span', null, locale);
    }
    expect(renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'uk' }, createElement(Probe)))).toContain('uk');
  });
});
