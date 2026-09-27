import { createContext, createElement, useCallback, useContext, useEffect, useState, type ReactNode } from 'react';

import { en, uk, type I18nKey } from './catalog.gen';

export type { I18nKey } from './catalog.gen';
export type I18nLocale = 'en' | 'uk';

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
  locale: I18nLocale;
  setLocale: (locale: I18nLocale) => void;
}

const LocaleContext = createContext<LocaleContextValue>({ locale: 'en', setLocale: () => undefined });

export function LocaleProvider({ children, initialLocale }: { children?: ReactNode; initialLocale?: I18nLocale }) {
  const [locale, setLocale] = useState<I18nLocale>(() => initialLocale ?? readSavedLocale());

  useEffect(() => {
    persistLocale(locale);
  }, [locale]);

  return createElement(LocaleContext.Provider, { value: { locale, setLocale } }, children);
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
  const template = locale === 'uk' && uk[typedKey] !== '' ? uk[typedKey] : en[typedKey];
  return template.replace(/\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (placeholder, name: string) => {
    const value = params?.[name];
    return value === undefined ? placeholder : String(value);
  });
}
