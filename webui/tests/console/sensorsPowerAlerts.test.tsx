import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import SensorsFeature from '../../src/features/sensors/SensorsFeature';
import type { DeviceStatus } from '../../src/domain/device';

/* MAX-245: over-current state from /api/status reaches the operator. */

function statusWith(overrides: {
  serial?: Record<string, unknown>;
  channels?: Array<Record<string, unknown>>;
}): DeviceStatus {
  return {
    system: {},
    power: {
      valid: true,
      voltage_v: 5,
      current_a: 0.3,
      power_w: 1.5,
      channels: overrides.channels ?? [],
    },
    environment: { valid: false, temperature_c: 0, humidity_percent: 0, pressure_pa: 0 },
    mux: { valid: false, active_channel: 0, active_pod: 0 },
    pods: [],
    acquisition: { serial: overrides.serial },
  } as unknown as DeviceStatus;
}

function render(status: DeviceStatus): string {
  return renderToStaticMarkup(createElement(SensorsFeature, { status, object: null, error: null }));
}

const channel = (name: string, id: number, overcurrent: string, latched: boolean, current: number) => ({
  id, name, valid: true, voltage_v: 5, current_a: current, power_w: 5 * current, overcurrent, overcurrent_latched: latched,
});

describe('Sensors power alerts', () => {
  it('shows an AD7606 over-current interlock trip with its current', () => {
    const html = render(statusWith({
      serial: { power_gate: 'overcurrent', power_fault_latched: true, power_fault_current_a: 0.42, power_fault_rail_v: 4.9, pattern: 'VARIED' },
    }));
    expect(html).toContain('OVER-CURRENT: AD7606 +5 V 0.420 A');
    expect(html).toContain('AD7606 over-current latched');
    /* Not mislabelled as an under-voltage fault. */
    expect(html).not.toContain('below the safe threshold');
  });

  it('keeps the under-voltage message for a rail-low trip', () => {
    const html = render(statusWith({
      serial: { power_gate: 'rail_low', power_fault_latched: true, power_fault_rail_v: 4.2, pattern: 'VARIED' },
    }));
    expect(html).toContain('POWER FAULT: AD7606 +5 V 4.20 V');
    expect(html).toContain('below the safe threshold');
    expect(html).not.toContain('over-current latched');
  });

  it('alerts for every INA3221 channel in warning or critical and not for healthy ones', () => {
    const html = render(statusWith({
      channels: [
        channel('BATTERY', 1, 'none', false, 0.4),
        channel('ESP32_5V', 2, 'warning', false, 0.85),
        channel('AD7606_5V', 3, 'critical', true, 0.3),
      ],
    }));
    expect(html).toContain('INA3221 ESP32_5V: over-current warning (0.850 A)');
    expect(html).toContain('INA3221 AD7606_5V: CRITICAL over-current (0.300 A), latched');
    expect(html).not.toContain('INA3221 BATTERY');
  });

  it('renders without channel data from older firmware', () => {
    const html = render(statusWith({ channels: undefined }));
    expect(html).not.toContain('INA3221');
  });
});
