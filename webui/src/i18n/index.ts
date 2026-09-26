import { en, uk, type I18nKey } from './catalog.gen';

export type { I18nKey } from './catalog.gen';
export type I18nLocale = 'en' | 'uk';

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
