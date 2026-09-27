import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import PowerBanner, { powerBannerText } from '../../src/components/PowerBanner';
import type { DeviceStatus } from '../../src/domain/device';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';

/* MAX-180/183: the low-battery banner follows the ESP32's decision. */

beforeAll(async () => { await loadUkrainianCatalog(); });

function withPower(power: Record<string, unknown> | undefined): DeviceStatus {
  return { power } as unknown as DeviceStatus;
}

describe('PowerBanner', () => {
  it.each(['en', 'uk'] as const)('renders the low-battery countdown in %s', (locale: I18nLocale) => {
    const status = withPower({ state: 'BATTERY_CRITICAL', low_battery: 'grace', shutdown_in_s: 7 });
    const expected = translate(locale, 'power.critical_grace', { seconds: formatNumber(locale, 7) });
    expect(powerBannerText(status, locale)).toBe(expected);
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(PowerBanner, { status })));
    expect(html).toContain(expected);
    expect(html).toContain('role="alert"');
    expect(/(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/.test(html.replace(/<[^>]*>/g, ' '))).toBe(false);
  });

  it.each(['en', 'uk'] as const)('keeps a missing shutdown countdown unknown in %s', (locale: I18nLocale) => {
    const missing = withPower({ state: 'BATTERY_CRITICAL', low_battery: 'grace' });
    const zero = withPower({ state: 'BATTERY_CRITICAL', low_battery: 'grace', shutdown_in_s: 0 });
    expect(powerBannerText(missing, locale)).toBe(translate(locale, 'power.critical_grace_unknown'));
    expect(powerBannerText(missing, locale)).not.toContain('0');
    expect(powerBannerText(zero, locale)).toBe(translate(locale, 'power.critical_grace', { seconds: formatNumber(locale, 0) }));
  });

  it('announces the running safe shutdown', () => {
    expect(powerBannerText(withPower({ state: 'BATTERY_CRITICAL', low_battery: 'shutdown' }), 'en'))
      .toBe(translate('en', 'power.critical_shutdown'));
    expect(renderToStaticMarkup(createElement(PowerBanner, {
      status: withPower({ state: 'BATTERY_CRITICAL', low_battery: 'shutdown' }),
    }))).toContain('role="alert"');
  });

  it('warns on a low battery', () => {
    expect(powerBannerText(withPower({ state: 'BATTERY_LOW', low_battery: 'warning' }), 'en')).toBe(translate('en', 'web.power.low'));
    expect(powerBannerText(withPower({ state: 'BATTERY_LOW' }), 'en')).toBe(translate('en', 'web.power.low'));
  });

  it('shows LOW, CRITICAL, shutdown countdown, and unknown telemetry', () => {
    const mappings: Array<[Record<string, unknown>, string, string]> = [
      [{ valid: true, state: 'BATTERY_LOW', low_battery: 'normal' }, translate('en', 'web.power.low'), 'alert'],
      [{ valid: true, state: 'BATTERY_CRITICAL', low_battery: 'normal' }, translate('en', 'power.critical_now'), 'alert'],
      [{ valid: true, state: 'BATTERY_LOW', low_battery: 'warning' }, translate('en', 'web.power.low'), 'alert'],
      [{ valid: true, state: 'BATTERY_CRITICAL', low_battery: 'grace', shutdown_in_s: 7 }, translate('en', 'power.critical_grace', { seconds: 7 }), 'alert'],
      [{ valid: true, state: 'BATTERY_CRITICAL', low_battery: 'shutdown' }, translate('en', 'power.critical_shutdown'), 'alert'],
      [{ valid: true, state: 'UNKNOWN', low_battery: 'normal' }, translate('en', 'power.status_unavailable'), 'status'],
      [{ valid: false, state: 'BATTERY', low_battery: 'normal' }, translate('en', 'power.status_unavailable'), 'status'],
      [{ valid: true, low_battery: 'normal' }, translate('en', 'power.status_unavailable'), 'status'],
    ];
    for (const [power, text, role] of mappings) {
      const status = withPower(power);
      expect(powerBannerText(status, 'en')).toBe(text);
      expect(renderToStaticMarkup(createElement(PowerBanner, { status }))).toContain(`role="${role}"`);
    }
  });

  it('hides normal battery, charging, charged, and USB-only states', () => {
    const states: Array<[string, Record<string, unknown>]> = [
      ['BATTERY', { battery_percent_valid: true, battery_percent_estimate: 48 }],
      ['USB_CHARGING', { battery_percent_valid: true, battery_percent_estimate: 61 }],
      ['USB_BATTERY_READY', { battery_full: true }],
      ['USB_BATTERY_READY', { battery_full: false }],
      ['USB_ONLY_NO_BATTERY', {}],
    ];
    for (const [state, details] of states) {
      const status = withPower({ valid: true, state, low_battery: 'normal', ...details });
      expect(powerBannerText(status, 'en')).toBeNull();
      expect(renderToStaticMarkup(createElement(PowerBanner, { status }))).toBe('');
    }
  });

  it('does not render a banner when no status object exists', () => {
    expect(powerBannerText(null, 'en')).toBeNull();
    expect(powerBannerText(withPower(undefined), 'en')).toBe(translate('en', 'power.status_unavailable'));
    expect(renderToStaticMarkup(createElement(PowerBanner, { status: null }))).toBe('');
  });
});
