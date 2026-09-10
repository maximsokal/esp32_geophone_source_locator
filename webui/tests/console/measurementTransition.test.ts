import { describe, expect, it } from 'vitest';
import type { DeviceStatus } from '../../src/domain/device';
import {
  isMeasurementTransitionComplete,
  measurementTransitionErrorMessage,
  waitForMeasurementTransition,
} from '../../src/features/objects/measurementTransition';

function status(state: string): DeviceStatus {
  return {
    system: {
      state: 'READY',
      mode: 'NORMAL',
      uptime_ms: 1,
      acquisition_ready: true,
      ready_pods: 4,
      required_pods: 4,
    },
    power: { valid: true, voltage_v: 5, current_a: 0, power_w: 0 },
    environment: { valid: true, temperature_c: 20, humidity_percent: 50, pressure_pa: 100000 },
    mux: { valid: true, active_channel: 1, active_pod: 1 },
    pods: [],
    acquisition: {
      running: true,
      requested_running: true,
      hardware_ready: true,
      source: 'AD7606',
      last_error: 0,
      last_error_name: 'ESP_OK',
      sample_rate_hz: 500,
      samples_recorded: 0,
      recording_ms: 0,
    },
    measurement: {
      available: true,
      state,
      object_id: 'OBJ_0001',
    },
  };
}

describe('measurement transition polling', () => {
  it('does not report start complete while the worker is still STARTING', async () => {
    const sequence = [status('STARTING'), status('RECORDING')];
    let reads = 0;
    const sleeps: number[] = [];

    expect(isMeasurementTransitionComplete('measurement_start', sequence[0])).toBe(false);
    const result = await waitForMeasurementTransition(
      'measurement_start',
      async () => sequence[Math.min(reads++, sequence.length - 1)],
      { intervalMs: 1, timeoutMs: 20, sleep: async (milliseconds) => { sleeps.push(milliseconds); } },
    );

    expect(result.measurement?.state).toBe('RECORDING');
    expect(reads).toBe(2);
    expect(sleeps).toEqual([1]);
  });

  it('does not refresh the stop result until STOPPING becomes REVIEW', async () => {
    const sequence = [status('STOPPING'), status('REVIEW')];
    let reads = 0;
    const readStatus = () => sequence[Math.min(reads++, sequence.length - 1)];
    let sessionsRefreshes = 0;

    const runStopAndRefresh = async () => {
      const result = await waitForMeasurementTransition(
        'measurement_stop',
        async () => readStatus(),
        { intervalMs: 1, timeoutMs: 20, sleep: async () => undefined },
      );
      sessionsRefreshes += 1;
      return result;
    };

    const pending = runStopAndRefresh();
    expect(sessionsRefreshes).toBe(0);
    const result = await pending;

    expect(sessionsRefreshes).toBe(1);
    expect(reads).toBe(2);
    expect(result.measurement?.state).toBe('REVIEW');
  });

  it('treats ERROR as terminal so the caller can refresh sessions before surfacing it', async () => {
    const result = await waitForMeasurementTransition(
      'measurement_stop',
      async () => status('ERROR'),
      { intervalMs: 1, timeoutMs: 20, sleep: async () => undefined },
    );

    expect(result.measurement?.state).toBe('ERROR');
    expect(measurementTransitionErrorMessage(result)).toBe('measurement service error');
  });
});
