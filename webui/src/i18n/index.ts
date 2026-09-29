import { createContext, createElement, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from 'react';

import { en } from './catalog.en.gen';
import type { I18nKey } from './catalog.keys.gen';

export type { I18nKey } from './catalog.keys.gen';
export type I18nLocale = 'en' | 'uk';

export type TranslatedErrorParam = string | number | Error | ((locale: I18nLocale) => string);

export class TranslatedError extends Error {
  readonly translationKey: I18nKey | null;
  readonly translationParams: Readonly<Record<string, TranslatedErrorParam>> | undefined;

  constructor(
    message: string,
    translationKey: I18nKey | null = null,
    translationParams?: Readonly<Record<string, TranslatedErrorParam>>,
  ) {
    super(message);
    this.name = 'TranslatedError';
    this.translationKey = translationKey;
    this.translationParams = translationParams;
  }

  static fromKey(
    key: I18nKey,
    params?: Readonly<Record<string, TranslatedErrorParam>>,
  ): TranslatedError {
    const messageParams: Record<string, string | number> = {};
    for (const [name, value] of Object.entries(params ?? {})) {
      messageParams[name] = value instanceof Error ? value.message : typeof value === 'function' ? value('en') : value;
    }
    return new TranslatedError(translate('en', key, messageParams), key, params);
  }
}

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

const EXACT_ERROR_KEYS: Readonly<Record<string, I18nKey>> = {
  'Saved seismic object and session are required.': 'api.error_saved_session_required',
  'Geolocation is not supported by this browser.': 'console.error.geolocation_unsupported',
  'measurement service error': 'console.error.measurement_failed',
  'recorder error': 'console.error.recorder_failed',
  'measurement service status unavailable': 'console.error.status_unavailable',
  'Trusted HTTPS is required for WebXR.': 'xr.error.secure_context',
  'WebXR is not available in this browser.': 'xr.error.webxr_unavailable',
  'XRWebGLLayer is unavailable.': 'xr.error.layer_unavailable',
  'Unable to create WebGL for WebXR.': 'xr.error.webgl_unavailable',
  'XRFrame.getDepthInformation() is unavailable.': 'xr.error.depth_api_unavailable',
  'WebXR tracking is running, but CPU depth was not enabled.': 'xr.error.cpu_depth_unavailable',
  'Recording in progress \u2014 scan storage is paused': 'scan.storage.paused',
  'Scan save paused for a critical control action. Retry to save it.': 'scan.storage.preempted',
  'Bulk transfer paused for a critical control action. Retry when ready.': 'scan.storage.preempted',
  'ESP scan save was not accepted.': 'api.scan_save_unaccepted',
  'scan info failed': 'api.scan_info_failed',
};

/** Translate WebUI-authored errors and add a localized prefix to opaque device/browser details. */
export function formatErrorText(locale: I18nLocale, reason: unknown): string {
  if (reason instanceof TranslatedError) {
    if (reason.translationKey === null) return reason.message;
    const params: Record<string, string | number> = {};
    for (const [name, value] of Object.entries(reason.translationParams ?? {})) {
      params[name] = value instanceof Error
        ? formatErrorText(locale, value)
        : typeof value === 'function' ? value(locale) : value;
    }
    return translate(locale, reason.translationKey, params);
  }
  const message = reason instanceof Error ? reason.message : String(reason);

  const exactKey = EXACT_ERROR_KEYS[message];
  if (exactKey !== undefined) {
    return translate(locale, exactKey);
  }

  let match = /^ESP API timeout \(([\s\S]*)\)$/.exec(message);
  if (match !== null) return translate(locale, 'api.error_request_timeout', { detail: match[1] });

  match = /^ESP API network error \(([^)]*)\): ([\s\S]*)$/.exec(message);
  if (match !== null) return translate(locale, 'api.error_request_failed', { detail: `${match[1]}: ${match[2]}` });

  match = /^ESP API (\d{3}) (\S+) (\S+): ([\s\S]*)$/.exec(message);
  if (match !== null) {
    return translate(locale, 'api.error_http', {
      status: match[1], request: `${match[2]} ${match[3]}`, detail: match[4],
    });
  }

  match = /^ESP scan (save|load) timeout \((\d+) ms\)$/.exec(message);
  if (match !== null) {
    return translate(locale, match[1] === 'save' ? 'api.scan_save_timeout' : 'api.scan_load_timeout', {
      duration: match[2],
    });
  }

  match = /^ESP scan (save|load) (?:network error: |\d+: )([\s\S]*)$/.exec(message);
  if (match !== null) {
    return translate(locale, match[1] === 'save' ? 'api.scan_save_failed' : 'api.scan_load_failed', {
      detail: match[2],
    });
  }

  match = /^ESP storage cache did not become idle within (\d+) ms\.$/.exec(message);
  if (match !== null) return translate(locale, 'console.error.cache_timeout', { duration: match[1] });

  match = /^ESP (measurement_start|measurement_stop) status became unavailable before (\S+): ([\s\S]*)$/.exec(message);
  if (match !== null) {
    return translate(locale, 'console.error.transition_status_unavailable', {
      action: match[1], target: match[2], detail: formatErrorText(locale, match[3]),
    });
  }

  match = /^ESP (measurement_start|measurement_stop) did not reach (\S+) within (\d+) ms\.$/.exec(message);
  if (match !== null) {
    return translate(locale, 'console.error.transition_timeout', {
      action: match[1], target: match[2], duration: match[3],
    });
  }

  match = /^ESP (measurement_start|measurement_stop) failed: ([\s\S]*)$/.exec(message);
  if (match !== null) {
    return translate(locale, 'console.error.transition_failed', {
      action: match[1], transitionError: formatErrorText(locale, match[2]),
    });
  }

  match = /^Acquisition unavailable: ([\s\S]*)$/.exec(message);
  if (match !== null) return translate(locale, 'console.error.acquisition_unavailable', { detail: match[1] });

  match = /^Phone GPS unavailable: ([\s\S]*)$/.exec(message);
  if (match !== null) return translate(locale, 'console.error.gps_unavailable', { detail: match[1] });

  match = /^Depth access failed: ([\s\S]*)$/.exec(message);
  if (match !== null) return translate(locale, 'xr.error.depth_access_failed', { detail: match[1] });

  if (message.startsWith('WebXR negotiated GPU-optimized depth,')) {
    return translate(locale, 'xr.error.gpu_depth_only');
  }

  return translate(locale, 'web.error_prefix', { detail: message });
}
