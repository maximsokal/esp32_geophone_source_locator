import { createContext, createElement, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from 'react';

import { en } from './catalog.en.gen';
import type { I18nKey } from './catalog.keys.gen';

export type { I18nKey } from './catalog.keys.gen';
export type I18nLocale = 'en' | 'uk';

let ukrainianCatalog: Record<I18nKey, string> | null = null;
let ukrainianCatalogPromise: Promise<void> | null = null;

export function loadUkrainianCatalog(): Promise<void> {
  if (ukrainianCatalog !== null) return Promise.resolve();
  if (ukrainianCatalogPromise === null) {
    ukrainianCatalogPromise = import('./catalog.uk.gen').then(({ uk }) => {
      ukrainianCatalog = uk;
    }).catch((error: unknown) => {
      ukrainianCatalogPromise = null;
      throw error;
    });
  }
  return ukrainianCatalogPromise;
}

const STORAGE_KEY = 'geolocator.locale';
const STORAGE_SCHEMA = 1;

export function readSavedLocale(): I18nLocale {
  try {
    const stored = window.localStorage.getItem(STORAGE_KEY);
    if (stored === null) return 'en';
    const value: unknown = JSON.parse(stored);
    if (typeof value !== 'object' || value === null || !('schema' in value) || !('locale' in value)) return 'en';
    const record = value as { schema: unknown; locale: unknown };
    return record.schema === STORAGE_SCHEMA && (record.locale === 'en' || record.locale === 'uk')
      ? record.locale
      : 'en';
  } catch {
    return 'en';
  }
}

export function persistLocale(locale: I18nLocale): void {
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ schema: STORAGE_SCHEMA, locale }));
  } catch {
    // Storage can be denied in private or restricted browser contexts.
  }
}

interface LocaleContextValue {
  /** Locale currently used to render every translated string and formatted value. */
  locale: I18nLocale;
  /** Requested locale; remains UK while its lazy catalog is loading. */
  selectedLocale: I18nLocale;
  localeLoading: boolean;
  localeLoadFailed: boolean;
  setLocale: (locale: I18nLocale) => void;
}

const LocaleContext = createContext<LocaleContextValue>({
  locale: 'en', selectedLocale: 'en', localeLoading: false, localeLoadFailed: false, setLocale: () => undefined,
});

export function LocaleProvider({ children, initialLocale }: { children?: ReactNode; initialLocale?: I18nLocale }) {
  const [selectedLocale, setSelectedLocale] = useState<I18nLocale>(() => initialLocale ?? readSavedLocale());
  const selectedLocaleRef = useRef(selectedLocale);
  const [locale, setActiveLocale] = useState<I18nLocale>(() =>
    selectedLocale === 'uk' && ukrainianCatalog !== null ? 'uk' : 'en');
  const [localeLoading, setLocaleLoading] = useState(() => selectedLocale === 'uk' && ukrainianCatalog === null);
  const [localeLoadFailed, setLocaleLoadFailed] = useState(false);

  useEffect(() => {
    persistLocale(selectedLocale);
  }, [selectedLocale]);

  useEffect(() => {
    if (selectedLocaleRef.current !== selectedLocale || selectedLocale === 'en') return;

    if (ukrainianCatalog !== null) {
      setActiveLocale('uk');
      setLocaleLoading(false);
      return;
    }

    let current = true;
    setLocaleLoading(true);
    void loadUkrainianCatalog().then(() => {
      if (current && selectedLocaleRef.current === 'uk') {
        setActiveLocale('uk');
        setLocaleLoading(false);
      }
    }).catch(() => {
      if (current && selectedLocaleRef.current === 'uk') {
        setActiveLocale('en');
        selectedLocaleRef.current = 'en';
        setSelectedLocale('en');
        setLocaleLoading(false);
        setLocaleLoadFailed(true);
      }
    });
    return () => { current = false; };
  }, [selectedLocale]);

  const setLocale = useCallback((next: I18nLocale) => {
    selectedLocaleRef.current = next;
    setSelectedLocale(next);
    setLocaleLoadFailed(false);
    if (next === 'en') {
      setActiveLocale('en');
      setLocaleLoading(false);
    } else if (ukrainianCatalog !== null) {
      setActiveLocale('uk');
      setLocaleLoading(false);
    } else {
      setLocaleLoading(true);
    }
  }, []);

  return createElement(LocaleContext.Provider, {
    value: { locale, selectedLocale, localeLoading, localeLoadFailed, setLocale },
  }, children);
}

export function useLocale(): LocaleContextValue {
  return useContext(LocaleContext);
}

export function useT(): (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string {
  const { locale } = useLocale();
  return useCallback((key: I18nKey, params?: Readonly<Record<string, string | number>>) => translate(locale, key, params), [locale]);
}

/** Missing parameters stay visible as {name}, so incomplete translations are diagnosable. */
export function translate(
  locale: I18nLocale,
  key: I18nKey,
  params?: Readonly<Record<string, string | number>>,
): string {
  if (typeof key !== 'string' || !Object.prototype.hasOwnProperty.call(en, key)) {
    return String(key);
  }

  const typedKey = key as I18nKey;
  const ukrainian = ukrainianCatalog?.[typedKey];
  const template = locale === 'uk' && ukrainian !== undefined && ukrainian !== '' ? ukrainian : en[typedKey];
  return template.replace(/\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (placeholder, name: string) => {
    const value = params?.[name];
    return value === undefined ? placeholder : String(value);
  });
}
