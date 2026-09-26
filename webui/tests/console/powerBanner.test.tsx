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
  });

  it('warns on a low battery', () => {
    expect(powerBannerText(withPower({ state: 'BATTERY_LOW', low_battery: 'warning' }))).toBe('Battery low: connect USB power soon.');
    expect(powerBannerText(withPower({ state: 'BATTERY_LOW' }))).toBe('Battery low: connect USB power soon.');
  });

  it('maps every stable JSON state to its WebUI text and appropriate live region', () => {
    const mappings: Array<[string, Record<string, unknown>, string, string]> = [
      ['USB_ONLY_NO_BATTERY', {}, 'USB power connected; no battery detected.', 'status'],
      ['USB_CHARGING', { battery_percent_valid: true, battery_percent_estimate: 61 }, 'Battery charging (61%).', 'status'],
      ['USB_BATTERY_READY', { battery_full: true }, 'Battery fully charged.', 'status'],
      ['USB_BATTERY_READY', { battery_full: false }, 'USB power connected; battery idle.', 'status'],
      ['BATTERY', { battery_percent_valid: true, battery_percent_estimate: 48 }, 'Running on battery (48%).', 'status'],
      ['BATTERY_LOW', {}, 'Battery low: connect USB power soon.', 'alert'],
      ['BATTERY_CRITICAL', {}, 'Battery critical: connect USB power now.', 'alert'],
      ['UNKNOWN', {}, 'Battery status unavailable.', 'status'],
    ];
    for (const [state, details, text, role] of mappings) {
      const status = withPower({ valid: true, state, low_battery: 'normal', ...details });
      expect(powerBannerText(status)).toBe(text);
      expect(renderToStaticMarkup(createElement(PowerBanner, { status }))).toContain(`role="${role}"`);
    }
  });

  it('never renders invalid or nonfinite SoC as a percentage', () => {
    expect(powerBannerText(withPower({ valid: true, state: 'BATTERY', battery_percent_valid: false, battery_percent_estimate: 0 })))
      .toBe('Running on battery.');
    expect(powerBannerText(withPower({ valid: true, state: 'BATTERY', battery_percent_valid: true, battery_percent_estimate: Number.NaN })))
      .toBe('Running on battery.');
    expect(powerBannerText(withPower({ valid: false, state: 'USB_BATTERY_READY', battery_full: true })))
      .toBe('Battery status unavailable.');
  });

  it('does not render a banner when no status object exists', () => {
    expect(powerBannerText(null)).toBeNull();
    expect(powerBannerText(withPower(undefined))).toBeNull();
    expect(renderToStaticMarkup(createElement(PowerBanner, { status: null }))).toBe('');
  });
});
