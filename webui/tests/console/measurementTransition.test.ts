import { describe, expect, it } from 'vitest';
import type { DeviceStatus } from '../../src/domain/device';
import {
  isMeasurementTransitionComplete,
  isStorageCacheIdle,
  measurementControlLocked,
  measurementOwnsRecorder,
  measurementTransitionErrorMessage,
  measurementTransitionNeedsSessionRefresh,
  scannerStorageActionsDisabled,
  waitForStorageCacheIdle,
  waitForMeasurementTransition,
} from '../../src/features/objects/measurementTransition';

function status(state: string, busy = false, storageBusy = false): DeviceStatus {
  return {
    storage_busy: storageBusy,
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
      busy,
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

  it('keeps polling when a retry still exposes the previous ERROR while busy', async () => {
    const sequence = [
      status('ERROR', true),
      status('STARTING', true),
      status('RECORDING', false),
    ];
    let reads = 0;
    const sleeps: number[] = [];

    const result = await waitForMeasurementTransition(
      'measurement_start',
      async () => sequence[reads++],
      { intervalMs: 1, timeoutMs: 20, sleep: async (milliseconds) => { sleeps.push(milliseconds); } },
    );

    expect(result.measurement?.state).toBe('RECORDING');
    expect(reads).toBe(3);
    expect(sleeps).toEqual([1, 1]);
  });

  it('treats ERROR as terminal only after the accepted retry releases busy', async () => {
    const sequence = [status('ERROR', true), status('ERROR', false)];
    let reads = 0;

    const result = await waitForMeasurementTransition(
      'measurement_start',
      async () => sequence[reads++],
      { intervalMs: 1, timeoutMs: 20, sleep: async () => undefined },
    );

    expect(result.measurement?.state).toBe('ERROR');
    expect(result.measurement?.busy).toBe(false);
    expect(reads).toBe(2);
    expect(measurementTransitionErrorMessage(result)).toBe('measurement service error');
  });

  it('refreshes sessions only after STOP, never at START completion', () => {
    expect(measurementTransitionNeedsSessionRefresh('measurement_start')).toBe(false);
    expect(measurementTransitionNeedsSessionRefresh('measurement_stop')).toBe(true);
  });

  it('uses measurement state as the recorder ownership source of truth', () => {
    expect(measurementOwnsRecorder(status('STARTING'))).toBe(true);
    expect(measurementOwnsRecorder(status('RECORDING'))).toBe(true);
    expect(measurementOwnsRecorder(status('STOPPING'))).toBe(true);
    expect(measurementOwnsRecorder(status('REVIEW'))).toBe(false);
    expect(measurementControlLocked(status('LIVE', true))).toBe(true);
  });

  it('disables scanner storage actions from the active measurement status', () => {
    expect(scannerStorageActionsDisabled(status('STARTING'))).toBe(true);
    expect(scannerStorageActionsDisabled(status('RECORDING'))).toBe(true);
    expect(scannerStorageActionsDisabled(status('STOPPING'))).toBe(true);
    expect(scannerStorageActionsDisabled(status('REVIEW'))).toBe(false);
    expect(scannerStorageActionsDisabled(null)).toBe(false);
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

  it('waits for the cache publish after REVIEW before session refresh', async () => {
    const sequence = [
      status('REVIEW', false, true),
      status('REVIEW', false, false),
    ];
    let reads = 0;
    const sleeps: number[] = [];

    expect(isStorageCacheIdle(sequence[0])).toBe(false);
    const result = await waitForStorageCacheIdle(
      async () => sequence[reads++],
      { intervalMs: 1, timeoutMs: 20, sleep: async (milliseconds) => { sleeps.push(milliseconds); } },
    );

    expect(result.storage_busy).toBe(false);
    expect(reads).toBe(2);
    expect(sleeps).toEqual([1]);
  });

  it('does not poll a cache reported unavailable by status', async () => {
    const unavailable = status('REVIEW', false, true);
    unavailable.storage_available = false;
    let reads = 0;
    const sleeps: number[] = [];

    expect(isStorageCacheIdle(unavailable)).toBe(true);
    await expect(waitForStorageCacheIdle(
      async () => { reads += 1; return unavailable; },
      { intervalMs: 1, timeoutMs: 20, sleep: async (milliseconds) => { sleeps.push(milliseconds); } },
    )).resolves.toBe(unavailable);
    expect(reads).toBe(1);
    expect(sleeps).toEqual([]);
  });

  it('treats ERROR as terminal so the caller can refresh sessions before surfacing it', async () => {
    const result = await waitForMeasurementTransition(
      'measurement_stop',
      async () => status('ERROR', false),
      { intervalMs: 1, timeoutMs: 20, sleep: async () => undefined },
    );

    expect(result.measurement?.state).toBe('ERROR');
    expect(measurementTransitionErrorMessage(result)).toBe('measurement service error');
  });
});
