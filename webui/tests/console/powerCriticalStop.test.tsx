import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { normalizeDeviceStatus } from '../../src/api/deviceStatusSchema';
import PowerBanner, { powerBannerText } from '../../src/components/PowerBanner';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';

/*
 * Critical battery stops the device. Whatever the operator is doing (a START that is waiting for
 * Channel Sounding, RANGING, a recording), the Web UI must say that the battery is critical and
 * that the device is stopping, in English and Ukrainian. The status below is what the firmware
 * sends, passed through the same schema the console uses.
 */

beforeAll(async () => { await loadUkrainianCatalog(); });

type Activity = { name: string; measurementState: string; workflowStep: string };

const ACTIVITIES: Activity[] = [
  { name: 'START waiting for geometry', measurementState: 'STARTING', workflowStep: 'RECORD_READY' },
  { name: 'ranging', measurementState: 'LIVE', workflowStep: 'RANGING' },
  { name: 'recording', measurementState: 'RECORDING', workflowStep: 'RECORDING' },
  { name: 'saving the recording after the stop', measurementState: 'STOPPING', workflowStep: 'RECORDING' },
];

function criticalStatus(activity: Activity, lowBattery: 'grace' | 'shutdown', seconds: number) {
  const raw = JSON.parse(JSON.stringify(fullDeviceStatus())) as Record<string, any>;
  raw.power = {
    ...raw.power,
    state: 'BATTERY_CRITICAL',
    usb_present: false,
    charging: false,
    battery_percent_valid: true,
    battery_percent_estimate: 2,
    low_battery: lowBattery,
    shutdown_in_s: seconds,
  };
  raw.measurement = { ...raw.measurement, state: activity.measurementState };
  raw.workflow = { ...raw.workflow, step: activity.workflowStep };
  return normalizeDeviceStatus(raw);
}

function render(status: ReturnType<typeof criticalStatus>, locale: I18nLocale): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(PowerBanner, { status })));
}

describe('critical battery stop in the Web UI', () => {
  it.each(ACTIVITIES)('says the device is stopping while $name (EN and UK)', (activity) => {
    const status = criticalStatus(activity, 'shutdown', 0);
    expect(status.power.low_battery).toBe('shutdown');
    for (const locale of ['en', 'uk'] as const) {
      const text = powerBannerText(status, locale);
      expect(text).toBe(translate(locale, 'power.critical_shutdown'));
      const html = render(status, locale);
      expect(html).toContain('role="alert"');
      // The wording itself, not only the catalog lookup: critical, stopping, data is being saved.
      if (locale === 'en') {
        expect(text).toMatch(/Battery critical/);
        expect(text).toMatch(/stopping/);
        expect(text).toMatch(/Saving the recording and the SD card/);
      } else {
        expect(text).toMatch(/Критичний заряд/);
        expect(text).toMatch(/зупинка/);
        expect(text).toMatch(/Запис і дані на SD-карті зберігаються/);
      }
      // No raw catalog keys leak into the page.
      expect(/(?:^|\s)[a-z_]+\.[a-z_.]+(?=\s|$)/.test(html.replace(/<[^>]*>/g, ' '))).toBe(false);
    }
  });

  it.each(ACTIVITIES)('warns that measuring will stop when USB does not arrive in time while $name', (activity) => {
    const status = criticalStatus(activity, 'grace', 8);
    for (const locale of ['en', 'uk'] as const) {
      const text = powerBannerText(status, locale);
      expect(text).toBe(translate(locale, 'power.critical_grace', { seconds: formatNumber(locale, 8) }));
      expect(render(status, locale)).toContain('role="alert"');
      if (locale === 'en') {
        expect(text).toMatch(/connect USB power within 8 s/);
        expect(text).toMatch(/stops measuring, saves the data and shuts down/);
      } else {
        expect(text).toMatch(/протягом 8 с/);
        expect(text).toMatch(/зупинить вимірювання, збереже дані й вимкнеться/);
      }
    }
  });

  it('shows nothing once USB brought the battery back to normal', () => {
    const raw = JSON.parse(JSON.stringify(fullDeviceStatus())) as Record<string, any>;
    raw.power = { ...raw.power, state: 'USB_CHARGING', low_battery: 'normal', shutdown_in_s: 0 };
    const status = normalizeDeviceStatus(raw);
    expect(powerBannerText(status, 'en')).toBeNull();
    expect(render(status, 'en')).toBe('');
  });
});
