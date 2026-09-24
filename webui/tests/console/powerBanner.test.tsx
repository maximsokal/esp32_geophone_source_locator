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

  it('stays silent when the battery is fine, on USB, or without data', () => {
    for (const state of ['BATTERY', 'USB_CHARGING', 'USB_BATTERY_READY', 'USB_ONLY_NO_BATTERY', 'UNKNOWN']) {
      expect(powerBannerText(withPower({ state, low_battery: 'normal' }))).toBeNull();
    }
    expect(powerBannerText(null)).toBeNull();
    expect(powerBannerText(withPower(undefined))).toBeNull();
    expect(renderToStaticMarkup(createElement(PowerBanner, { status: null }))).toBe('');
  });
});
