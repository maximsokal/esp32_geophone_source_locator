import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import SensorsFeature from '../../src/features/sensors/SensorsFeature';
import type { DevicePod, DeviceStatus } from '../../src/domain/device';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';

/* MAX-245: over-current state from /api/status reaches the operator. */

function statusWith(overrides: {
  serial?: Record<string, unknown>;
  channels?: Array<Record<string, unknown>>;
  pods?: DevicePod[];
  acquisition?: Record<string, unknown>;
  hardware?: Record<string, unknown>;
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
    pods: overrides.pods ?? [],
    acquisition: { ...overrides.acquisition, serial: overrides.serial },
    hardware: overrides.hardware,
  } as unknown as DeviceStatus;
}

function render(status: DeviceStatus, locale: I18nLocale = 'en'): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(SensorsFeature, {
    status, object: null, error: null, onCalibrateHeights: async () => true,
  })));
}

beforeAll(async () => { await loadUkrainianCatalog(); });

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

  it.each(['en', 'uk'] as const)('shows BMP280 temperature but unavailable humidity in %s', (locale) => {
    const pod = {
      id: 1, state: 'CONNECTED',
      capabilities: { barometer: true, environment: true, imu: true, geophone: true, local_position: true, global_position: false },
      barometer: { valid: true, pressure_pa: 100000, relative_altitude_cm: 0, reference_pressure_pa: 0 },
      environment: { valid: true, temperature_valid: true, temperature_c: 24.25, humidity_valid: false, humidity_percent: 0 },
      imu: { valid: false, roll_deg: 0, pitch_deg: 0, yaw_relative_deg: 0 },
      geophone: { valid: false, instant_uv: 0, rms_uv: 0, peak_uv: 0, noise_rms_uv: 0, snr_db: 0 },
      position: { local_valid: false, local_x_m: 0, local_y_m: 0, global_valid: false, latitude: 0, longitude: 0, altitude_valid: false, altitude_m: 0 },
    } as DevicePod;
    const html = render(statusWith({ pods: [pod] }), locale);
    expect(html).toContain(`<dt>${translate(locale, 'sensors.humidity')}</dt><dd>${translate(locale, 'common.not_available')}</dd>`);
    expect(html).toContain(`${formatNumber(locale, 24.25, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} \u00b0C`);
    expect(html).not.toContain('100.0 %');
  });

  it.each(['en', 'uk'] as const)('shows unknown markers for invalid zero-valued sensor metrics in %s', (locale) => {
    const pod: DevicePod = {
      id: 1,
      state: 'CONNECTED',
      capabilities: { barometer: true, environment: true, imu: true, geophone: true, local_position: true, global_position: false },
      barometer: { valid: false, pressure_pa: 0, relative_altitude_cm: 0, reference_pressure_pa: 0 },
      environment: { valid: false, temperature_valid: false, temperature_c: 0, humidity_valid: false, humidity_percent: 0 },
      imu: { valid: false, roll_deg: 0, pitch_deg: 0, yaw_relative_deg: 0 },
      geophone: { valid: false, instant_uv: 0, rms_uv: 0, peak_uv: 0, noise_rms_uv: 0, snr_db: 0 },
      position: { local_valid: false, local_x_m: 0, local_y_m: 0, global_valid: false, latitude: 0, longitude: 0, altitude_valid: false, altitude_m: 0 },
    };
    const html = render(statusWith({ pods: [pod] }), locale);
    const unavailable = translate(locale, 'common.not_available');

    for (const key of ['sensors.pressure', 'sensors.temperature', 'sensors.humidity'] as const) {
      expect(html).toContain(`<dt>${translate(locale, key)}</dt><dd>${unavailable}</dd>`);
    }
    expect(html).not.toContain(`<dd>${formatNumber(locale, 0, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} %</dd>`);
  });
});

describe('Sensors AD7606 status', () => {
  it.each([
    ['ESP_OK error fields', { running: false, hardware_ready: false, last_error: 0, last_error_name: 'ESP_OK' }],
    ['missing error fields', { running: false, hardware_ready: false }],
  ] as const)('reports an unstarted ADC for unknown inventory state with %s', (_case, acquisition) => {
    const html = render(statusWith({
      acquisition,
      hardware: {
        modules: [{ id: 'ad7606', state: 'unknown', required: true, reason: '' }],
        required_missing: false,
      },
    }));

    expect(html).toContain('AD7606 not started yet');
    expect(html).not.toContain('NOT DETECTED');
  });

  it('shows the acquisition error name when AD7606 startup failed', () => {
    const html = render(statusWith({
      acquisition: { running: false, hardware_ready: false, last_error: 123, last_error_name: 'ESP_ERR_TIMEOUT' },
      hardware: {
        modules: [{ id: 'ad7606', state: 'unknown', required: true, reason: '' }],
        required_missing: false,
      },
    }));

    expect(html).toContain('AD7606 failed: ESP_ERR_TIMEOUT');
  });

  it('shows not detected only when inventory explicitly marks AD7606 absent', () => {
    const html = render(statusWith({
      acquisition: { running: false, hardware_ready: false, last_error: 0, last_error_name: 'ESP_OK' },
      hardware: {
        modules: [{ id: 'ad7606', state: 'absent', required: true, reason: '' }],
        required_missing: true,
      },
    }));

    expect(html).toContain('NOT DETECTED');
    expect(html).not.toContain('AD7606 not started yet');
  });

  it('preserves the legacy hardware-ready label when old firmware omits new status fields', () => {
    const html = render(statusWith({ acquisition: { running: false, hardware_ready: false } }));

    expect(html).toContain('NOT DETECTED');
  });
});
