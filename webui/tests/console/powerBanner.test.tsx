import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import PowerBanner, { powerBannerText } from '../../src/components/PowerBanner';
import type { DeviceStatus } from '../../src/domain/device';

/* MAX-180/183: the low-battery banner follows the ESP32's decision. */

function withPower(power: Record<string, unknown> | undefined): DeviceStatus {
  return { power } as unknown as DeviceStatus;
}

describe('PowerBanner', () => {
  it('counts down to the low-battery shutdown', () => {
    const status = withPower({ state: 'BATTERY_CRITICAL', low_battery: 'grace', shutdown_in_s: 7 });
    expect(powerBannerText(status)).toBe('Battery critical: connect USB power within 7 s or the device shuts down.');
    expect(renderToStaticMarkup(createElement(PowerBanner, { status }))).toContain('role="alert"');
  });

  it('announces the running safe shutdown', () => {
    expect(powerBannerText(withPower({ state: 'BATTERY_CRITICAL', low_battery: 'shutdown' })))
      .toContain('shutting down safely');
    expect(renderToStaticMarkup(createElement(PowerBanner, {
      status: withPower({ state: 'BATTERY_CRITICAL', low_battery: 'shutdown' }),
    }))).toContain('role="alert"');
  });

  it('warns on a low battery', () => {
    expect(powerBannerText(withPower({ state: 'BATTERY_LOW', low_battery: 'warning' }))).toBe('Battery low: connect USB power soon.');
    expect(powerBannerText(withPower({ state: 'BATTERY_LOW' }))).toBe('Battery low: connect USB power soon.');
  });

  it('shows LOW, CRITICAL, shutdown countdown, and unknown telemetry', () => {
    const mappings: Array<[Record<string, unknown>, string, string]> = [
      [{ valid: true, state: 'BATTERY_LOW', low_battery: 'normal' }, 'Battery low: connect USB power soon.', 'alert'],
      [{ valid: true, state: 'BATTERY_CRITICAL', low_battery: 'normal' }, 'Battery critical: connect USB power now.', 'alert'],
      [{ valid: true, state: 'BATTERY_LOW', low_battery: 'warning' }, 'Battery low: connect USB power soon.', 'alert'],
      [{ valid: true, state: 'BATTERY_CRITICAL', low_battery: 'grace', shutdown_in_s: 7 }, 'Battery critical: connect USB power within 7 s or the device shuts down.', 'alert'],
      [{ valid: true, state: 'BATTERY_CRITICAL', low_battery: 'shutdown' }, 'Battery critical: the device is shutting down safely (recording finalised, SD flushed).', 'alert'],
      [{ valid: true, state: 'UNKNOWN', low_battery: 'normal' }, 'Battery status unavailable.', 'status'],
      [{ valid: false, state: 'BATTERY', low_battery: 'normal' }, 'Battery status unavailable.', 'status'],
      [{ valid: true, low_battery: 'normal' }, 'Battery status unavailable.', 'status'],
    ];
    for (const [power, text, role] of mappings) {
      const status = withPower(power);
      expect(powerBannerText(status)).toBe(text);
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
      expect(powerBannerText(status)).toBeNull();
      expect(renderToStaticMarkup(createElement(PowerBanner, { status }))).toBe('');
    }
  });

  it('does not render a banner when no status object exists', () => {
    expect(powerBannerText(null)).toBeNull();
    expect(powerBannerText(withPower(undefined))).toBe('Battery status unavailable.');
    expect(renderToStaticMarkup(createElement(PowerBanner, { status: null }))).toBe('');
  });
});
