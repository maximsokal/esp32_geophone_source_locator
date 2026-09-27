import type { I18nLocale } from './index';

const intlLocale: Record<I18nLocale, string> = { en: 'en-US', uk: 'uk-UA' };

export function formatNumber(locale: I18nLocale, value: number, options?: Intl.NumberFormatOptions): string {
  return new Intl.NumberFormat(intlLocale[locale], options).format(value).replace(/\u202f/g, '\u00a0');
}

export function formatPercent(locale: I18nLocale, value: number, maximumFractionDigits = 0): string {
  return formatNumber(locale, value, { style: 'percent', maximumFractionDigits });
}

export function formatDistance(locale: I18nLocale, meters: number, fractionDigits = 2): string {
  return new Intl.NumberFormat(intlLocale[locale], {
    style: 'unit',
    unit: 'meter',
    unitDisplay: 'short',
    minimumFractionDigits: fractionDigits,
    maximumFractionDigits: fractionDigits,
  }).format(meters).replace(/\u202f/g, '\u00a0');
}

export function formatDate(locale: I18nLocale, value: Date | number): string {
  return new Intl.DateTimeFormat(intlLocale[locale], { dateStyle: 'medium', timeStyle: 'short' }).format(value).replace(/\u202f/g, '\u00a0');
}
