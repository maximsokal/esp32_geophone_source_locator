import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

import { DeviceStatusValidationError, normalizeDeviceStatus } from '../../src/api/deviceStatusSchema';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';

const REQUIRED_SECTIONS = ['system', 'power', 'environment', 'mux', 'pods', 'acquisition'] as const;
const OPTIONAL_SECTIONS = [
  'storage_available', 'storage_busy', 'hardware', 'recording', 'recording_gate', 'measurement',
  'object_store', 'recording_sessions', 'workflow', 'ranging', 'nrf_logs', 'main',
] as const;
const OPTIONAL_INNER_FIELDS: Array<[string, unknown]> = [
  ['pods.0.capabilities', []],
  ['pods.0.height_reference', []],
  ['pods.0.environment.temperature_valid', 1],
  ['pods.0.environment.humidity_valid', 1],
  ['power.battery_present', 'yes'],
  ['power.battery_percent_valid', 'yes'],
  ['power.battery_percent_estimate', '42'],
  ['power.battery_voltage_v', '4.1'],
  ['power.battery_current_a', '1'],
  ['power.usb_present', 'yes'],
  ['power.charging', 'yes'],
  ['power.battery_full', 'yes'],
  ['power.low_battery', 1],
  ['power.shutdown_in_s', '60'],
  ['power.battery_soc_source', 1],
  ['power.state', 3],
  ['power.channels', 1],
  ['power.channels.0.overcurrent', 1],
  ['power.channels.0.overcurrent_latched', 1],
  ['acquisition.last_error', '0'],
  ['acquisition.last_error_name', 0],
  ['acquisition.serial.quality', 3],
  ['acquisition.serial.stream_state', 3],
  ['acquisition.serial.stream_stuck', 3],
  ['acquisition.serial.power_gate', 3],
  ['acquisition.serial.power_fault_latched', 3],
  ['acquisition.serial.power_fault_rail_v', '5'],
  ['acquisition.serial.power_fault_current_a', '1'],
  ['acquisition.serial.repeated_frame_streak', '3'],
  ['acquisition.serial.all_zero_frames', '3'],
  ['acquisition.serial.all_ff_frames', '3'],
  ['acquisition.serial.all_identical_frames', '3'],
  ['acquisition.serial.varied_frames', '3'],
  ['acquisition.serial.digital_interference_frames', '3'],
  ['acquisition.serial.quality_window_frames', '3'],
  ['acquisition.serial.quality_bad_ratio', '0.1'],
  ['acquisition.serial.quality_all_00_ratio', '0.1'],
  ['acquisition.serial.quality_all_ff_ratio', '0.1'],
  ['acquisition.serial.quality_all_identical_ratio', '0.1'],
  ['acquisition.serial.quality_repeated_ratio', '0.1'],
  ['acquisition.serial.quality_digital_interference_ratio', '0.1'],
  ['acquisition.signal.channel_count', '4'],
  ['acquisition.signal.quality', 3],
  ['acquisition.signal.rail_hit_frames', '3'],
  ['acquisition.signal.clip_frames', '3'],
  ['acquisition.signal.quality_window_frames', '3'],
  ['acquisition.signal.rail_hit_ratio', '0.1'],
  ['acquisition.signal.clip_ratio', '0.1'],
  ['main.height_reference', []],
  ['main.humidity_mpct', '50000'],
  ['main.sensor_present', []],
  ['main.sensor_valid', []],
  ['hardware.modules.0.reason_code', 3],
  ['recording.running', 'yes'],
  ['recording.state', 1],
  ['recording.stop_stage', 1],
  ['recording.stop_error_stage', 1],
  ['recording.object_id', 1],
  ['recording.session_id', 1],
  ['recording.source', 1],
  ['recording.last_error_name', 1],
  ['recording.error', 1],
  ['recording.stop_stage_error', '0'],
  ['recording.sample_rate_hz', '500'],
  ['recording.channel_count', '4'],
  ['recording.samples_written', '10'],
  ['recording.dropped_frames', '0'],
  ['recording.recording_ms', '10'],
  ['recording.last_error', '0'],
  ['recording_gate.assigned_count', '1'],
  ['recording_gate.accepted_count', '1'],
  ['recording_gate.decision', 1],
  ['recording_gate.dimensionality', 1],
  ['recording_gate.reason', 1],
  ['recording_gate.warning', 1],
  ['recording_gate.error', 1],
  ['recording_gate.allowed', 'yes'],
  ['recording_gate.nodes', 1],
  ['recording_gate.nodes.0.tilt_deg', '2.0'],
  ['measurement.initialized', 'yes'],
  ['measurement.busy', 'yes'],
  ['measurement.active_object_present', 'yes'],
  ['measurement.state', 1],
  ['measurement.object_id', 1],
  ['measurement.object_name', 1],
  ['measurement.session_id', 1],
  ['measurement.stop_stage', 1],
  ['measurement.stop_error_stage', 1],
  ['measurement.last_error_name', 1],
  ['measurement.error', 1],
  ['measurement.stop_stage_error', '0'],
  ['measurement.samples_written', '10'],
  ['measurement.dropped_frames', '0'],
  ['measurement.recording_ms', '10'],
  ['measurement.last_error', '0'],
  ['object_store.error', 1],
  ['recording_sessions.error', 1],
  ['recording_sessions.sessions.0.status', 1],
  ['recording_sessions.sessions.0.readable', 'yes'],
  ['recording_sessions.sessions.0.recovered', 'yes'],
  ['recording_sessions.sessions.0.source', 1],
  ['recording_sessions.sessions.0.dropped_frames_available', 'true'],
  ['recording_sessions.sessions.0.recording_ms_available', 'true'],
  ['workflow.sensor_sufficiency', 3],
  ['workflow.assigned_remotes', '1'],
  ['workflow.reason', 3],
  ['workflow.has_scan', 1],
  ['workflow.calc_settings_ready', 1],
  ['workflow.calculation_class', 3],
  ['workflow.object_id', 1],
  ['workflow.session_id', 1],
  ['workflow.calibration', null],
  ['workflow.placement', []],
];
const FLOAT_NULL_PATHS = [
  'power.voltage_v',
  'environment.temperature_c',
  'pods.0.geophone.snr_db',
  'pods.0.position.latitude',
  'pods.0.barometer.pressure_pa',
  'power.channels.0.current_a',
  'power.battery_percent_estimate',
  'acquisition.serial.power_fault_current_a',
  'acquisition.serial.quality_bad_ratio',
  'acquisition.serial.quality_all_00_ratio',
  'acquisition.serial.quality_all_ff_ratio',
  'acquisition.serial.quality_all_identical_ratio',
  'acquisition.serial.quality_repeated_ratio',
  'acquisition.serial.quality_digital_interference_ratio',
  'acquisition.signal.rail_hit_ratio',
  'acquisition.signal.clip_ratio',
  'recording_gate.nodes.0.tilt_deg',
  'object_store.objects.0.location.latitude',
  'object_store.objects.0.cable_lengths_m.0',
  'workflow.placement.per_node.0.altitude_asl_m',
] as const;
const REQUIRED_INTEGER_PATHS = [
  'system.uptime_ms',
  'system.ready_pods',
  'pods.0.id',
  'mux.active_channel',
  'acquisition.samples_recorded',
] as const;

type TestRecord = Record<string, unknown>;
type TestContainer = TestRecord | unknown[];

function isRecord(value: unknown): value is TestRecord {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function parentAt(root: unknown, path: string): { parent: TestContainer; key: string } {
  const parts = path.split('.');
  let current = root;
  for (let index = 0; index < parts.length - 1; index++) {
    const part = parts[index];
    if (Array.isArray(current)) current = current[Number(part)];
    else if (isRecord(current)) current = current[part];
    else throw new Error(`Invalid test path: ${path}`);
  }
  if (!Array.isArray(current) && !isRecord(current)) throw new Error(`Invalid test path: ${path}`);
  return { parent: current, key: parts[parts.length - 1] };
}

function setAt(root: unknown, path: string, value: unknown): void {
  const { parent, key } = parentAt(root, path);
  if (Array.isArray(parent)) parent[Number(key)] = value;
  else parent[key] = value;
}

function deleteAt(root: unknown, path: string): void {
  const { parent, key } = parentAt(root, path);
  if (Array.isArray(parent)) delete parent[Number(key)];
  else delete parent[key];
}

function valueAt(root: unknown, path: string): unknown {
  const { parent, key } = parentAt(root, path);
  return Array.isArray(parent) ? parent[Number(key)] : parent[key];
}

function freshRaw(): unknown {
  return structuredClone(fullDeviceStatus());
}

function expectInvalid(raw: unknown, path: string): DeviceStatusValidationError {
  let thrown: unknown;
  try {
    normalizeDeviceStatus(raw);
  }
  catch (error) {
    thrown = error;
  }
  expect(thrown).toBeInstanceOf(DeviceStatusValidationError);
  if (!(thrown instanceof DeviceStatusValidationError)) throw new Error('Expected DeviceStatusValidationError');
  expect(thrown.path).toBe(path);
  return thrown;
}

describe('normalizeDeviceStatus', () => {
  it('accepts a full status without changing it', () => {
    const input = fullDeviceStatus();
    const normalized = normalizeDeviceStatus(input);

    expect(normalized).toBe(input);
    expect(normalized).toEqual(input);
  });

  it.each(REQUIRED_SECTIONS)('rejects a missing required section: %s', (section) => {
    const raw = freshRaw();
    deleteAt(raw, section);
    expectInvalid(raw, section);
  });

  it.each(['system', 'power', 'environment', 'mux', 'acquisition'] as const)(
    'rejects null, array, string, and number in required object section %s',
    (section) => {
      for (const value of [null, [], 'bad', 42]) {
        const raw = freshRaw();
        setAt(raw, section, value);
        expectInvalid(raw, section);
      }
    },
  );

  it('rejects non-array pods values and malformed array entries', () => {
    for (const value of [null, {}, 'bad', 42]) {
      const raw = freshRaw();
      setAt(raw, 'pods', value);
      expectInvalid(raw, 'pods');
    }
    const raw = freshRaw();
    setAt(raw, 'pods', [null]);
    expectInvalid(raw, 'pods.0');
    for (const value of [[], 'bad']) {
      const invalidPod = freshRaw();
      setAt(invalidPod, 'pods', [value]);
      expectInvalid(invalidPod, 'pods.0');
    }
  });

  it.each([
    ['power.voltage_v', NaN],
    ['power.voltage_v', Infinity],
    ['power.voltage_v', -Infinity],
    ['power.voltage_v', true],
    ['power.valid', 1],
    ['system.state', 1],
    ['system.acquisition_ready', 1],
    ['pods.0.id', NaN],
    ['pods.0.barometer', []],
    ['pods.0.environment.temperature_valid', 0],
  ] as const)('rejects an invalid primitive at %s', (path, value) => {
    const raw = freshRaw();
    setAt(raw, path, value);
    expectInvalid(raw, path);
  });

  it.each([null, undefined, [], 'bad', 42])('rejects a non-object root: %s', (raw) => {
    expectInvalid(raw, '$');
  });

  it('rejects malformed JSON-parsed status objects', () => {
    expectInvalid(JSON.parse('{}'), 'system');
    expectInvalid(JSON.parse('{"system":null}'), 'system');
  });

  it('accepts firmware null for battery_percent_estimate', () => {
    const raw = freshRaw();
    setAt(raw, 'power.battery_percent_estimate', null);
    expect(normalizeDeviceStatus(raw)).toBe(raw);
  });

  it.each(FLOAT_NULL_PATHS)('preserves cJSON null for float field %s', (path) => {
    const raw = freshRaw();
    setAt(raw, path, null);

    const normalized = normalizeDeviceStatus(raw);
    expect(normalized).toBe(raw);
    expect(valueAt(normalized, path)).toBeNull();
    expect(normalizeDeviceStatus(normalized)).toBe(normalized);
  });

  it.each(FLOAT_NULL_PATHS)('rejects malformed values for float field %s', (path) => {
    for (const value of [NaN, Infinity, -Infinity, '3.3', true, {}, []]) {
      const raw = freshRaw();
      setAt(raw, path, value);
      expectInvalid(raw, path);
    }
  });

  it.each(REQUIRED_INTEGER_PATHS)('rejects null for integer field %s', (path) => {
    const raw = freshRaw();
    setAt(raw, path, null);
    expectInvalid(raw, path);
  });

  it.each(OPTIONAL_INNER_FIELDS)('accepts an omitted firmware field and shape-checks it when present: %s', (path, wrongValue) => {
    const missing = freshRaw();
    deleteAt(missing, path);
    expect(normalizeDeviceStatus(missing)).toBe(missing);

    const malformed = freshRaw();
    setAt(malformed, path, wrongValue);
    expectInvalid(malformed, path);
  });

  it('accepts the older pod, acquisition, and workflow variants', () => {
    const raw = freshRaw();
    for (const path of [
      'pods.0.capabilities',
      'pods.0.height_reference',
      'pods.0.environment.temperature_valid',
      'pods.0.environment.humidity_valid',
      'power.battery_percent_estimate',
      'power.state',
      'power.channels.0.overcurrent',
      'power.channels.0.overcurrent_latched',
      'power.channels',
      'acquisition.cadence',
      'acquisition.serial.quality',
      'acquisition.serial.stream_state',
      'acquisition.serial.stream_stuck',
      'acquisition.serial.power_gate',
      'acquisition.serial.power_fault_latched',
      'acquisition.serial.power_fault_rail_v',
      'acquisition.serial.power_fault_current_a',
      'acquisition.serial.repeated_frame_streak',
      'acquisition.serial.all_zero_frames',
      'acquisition.serial.all_ff_frames',
      'acquisition.serial.all_identical_frames',
      'acquisition.serial.varied_frames',
      'acquisition.serial.digital_interference_frames',
      'acquisition.serial.quality_window_frames',
      'acquisition.serial.quality_bad_ratio',
      'acquisition.serial.quality_digital_interference_ratio',
      'acquisition.signal.channel_count',
      'acquisition.signal.quality',
      'acquisition.signal.rail_hit_frames',
      'acquisition.signal.clip_frames',
      'acquisition.signal.quality_window_frames',
      'acquisition.signal.rail_hit_ratio',
      'acquisition.signal.clip_ratio',
      'main.height_reference',
      'main.humidity_mpct',
      'hardware.modules.0.reason_code',
      'recording_sessions.sessions.0.dropped_frames_available',
      'recording_sessions.sessions.0.recording_ms_available',
      'workflow.sensor_sufficiency',
      'workflow.assigned_remotes',
      'workflow.reason',
      'workflow.has_scan',
      'workflow.calc_settings_ready',
      'workflow.calculation_class',
      'workflow.calibration',
      'workflow.placement',
    ]) {
      deleteAt(raw, path);
    }

    expect(normalizeDeviceStatus(raw)).toBe(raw);

    const noNewAcquisitionGroups = freshRaw();
    deleteAt(noNewAcquisitionGroups, 'acquisition.cadence');
    deleteAt(noNewAcquisitionGroups, 'acquisition.serial');
    deleteAt(noNewAcquisitionGroups, 'acquisition.signal');
    expect(normalizeDeviceStatus(noNewAcquisitionGroups)).toBe(noNewAcquisitionGroups);
  });

  it('accepts firmware error and unavailable variants without omitted optional fields', () => {
    const raw = freshRaw();
    setAt(raw, 'recording', { available: false, error: 'ESP_ERR_INVALID_STATE' });
    setAt(raw, 'recording_gate', { available: false, error: 'ESP_ERR_INVALID_STATE' });
    setAt(raw, 'measurement', { available: false, error: 'ESP_ERR_INVALID_STATE' });
    setAt(raw, 'recording_sessions.available', false);
    setAt(raw, 'recording_sessions.sessions', []);
    setAt(raw, 'recording_sessions.error', 'ESP_ERR_NOT_FOUND');
    setAt(raw, 'object_store.available', false);
    setAt(raw, 'object_store.active_id', null);
    setAt(raw, 'object_store.default_cable_lengths_m', []);
    setAt(raw, 'object_store.objects', []);
    setAt(raw, 'object_store.error', 'ESP_ERR_NOT_FOUND');
    setAt(raw, 'ranging.available', false);
    setAt(raw, 'ranging.error', 'ESP_ERR_INVALID_STATE');
    deleteAt(raw, 'ranging.progress');
    setAt(raw, 'main.sensor_present.aht20', false);
    setAt(raw, 'main.sensor_valid.aht20', false);
    deleteAt(raw, 'main.humidity_mpct');

    expect(normalizeDeviceStatus(raw)).toBe(raw);

    const invalidTilt = freshRaw();
    deleteAt(invalidTilt, 'recording_gate.nodes.0.tilt_deg');
    expect(normalizeDeviceStatus(invalidTilt)).toBe(invalidTilt);

    const unavailableLevelingNodes = freshRaw();
    deleteAt(unavailableLevelingNodes, 'recording_gate.nodes');
    expect(normalizeDeviceStatus(unavailableLevelingNodes)).toBe(unavailableLevelingNodes);
  });

  it('requires workflow details only on steps that read them and requires available ranging progress', () => {
    const calibrationStep = freshRaw();
    setAt(calibrationStep, 'workflow.step', 'CALIB_RESULT');
    deleteAt(calibrationStep, 'workflow.calibration');
    expectInvalid(calibrationStep, 'workflow.calibration');

    const placementStep = freshRaw();
    setAt(placementStep, 'workflow.step', 'PLACEMENT');
    deleteAt(placementStep, 'workflow.placement');
    expectInvalid(placementStep, 'workflow.placement');

    const missingProgress = freshRaw();
    deleteAt(missingProgress, 'ranging.progress');
    expectInvalid(missingProgress, 'ranging.progress');

    const incompleteProgress = freshRaw();
    deleteAt(incompleteProgress, 'ranging.progress.failure_reason');
    expectInvalid(incompleteProgress, 'ranging.progress.failure_reason');
  });

  it('preserves explicit firmware nulls in workflow measurements', () => {
    const raw = freshRaw();
    setAt(raw, 'object_store.active_id', null);
    setAt(raw, 'workflow.progress', null);
    setAt(raw, 'workflow.object_id', null);
    setAt(raw, 'workflow.session_id', null);
    setAt(raw, 'workflow.calibration.result', null);
    setAt(raw, 'workflow.calibration.cs.max_spread_mm', null);
    setAt(raw, 'workflow.calibration.per_node', [{
      node: 'remote1',
      height: { state: 'not_established', relative_z_mm: null },
      imu: { state: 'not_calibrated', gyro_bias_mdps: [null, null, null], gravity_ref_mg: [null, null, null] },
      cs_pairs_ok: false,
    }]);
    setAt(raw, 'workflow.placement.per_node', [{
      node: 'remote1',
      vertical: false,
      height_rel_main_mm: null,
      altitude_asl_m: null,
      stable: false,
    }]);

    const normalized = normalizeDeviceStatus(raw);
    expect(normalized).toBe(raw);
    expect(valueAt(normalized, 'object_store.active_id')).toBeNull();
    expect(valueAt(normalized, 'workflow.progress')).toBeNull();
    expect(valueAt(normalized, 'workflow.calibration.per_node.0.imu.gyro_bias_mdps.0')).toBeNull();
    expect(valueAt(normalized, 'workflow.placement.per_node.0.altitude_asl_m')).toBeNull();
  });

  it('preserves additive fields at the root and in known sections', () => {
    const raw = freshRaw();
    setAt(raw, 'future_top_level', { enabled: true });
    setAt(raw, 'system.future_system', 'kept');
    setAt(raw, 'power.future_power', 12);
    setAt(raw, 'pods.0.future_pod', ['kept']);
    setAt(raw, 'acquisition.future_acquisition', false);

    const normalized = normalizeDeviceStatus(raw);
    expect(normalized).toBe(raw);
    expect(normalized).toMatchObject({
      future_top_level: { enabled: true },
    });
    expect(normalized.system).toMatchObject({ future_system: 'kept' });
    expect(normalized.power).toMatchObject({ future_power: 12 });
    expect(normalized.pods[0]).toMatchObject({ future_pod: ['kept'] });
    expect(normalized.acquisition).toMatchObject({ future_acquisition: false });
  });

  it.each(OPTIONAL_SECTIONS)('keeps optional section absent: %s', (section) => {
    const raw = freshRaw();
    deleteAt(raw, section);
    const normalized = normalizeDeviceStatus(raw);
    expect(Object.hasOwn(normalized, section)).toBe(false);
  });

  it('accepts an old-firmware status with only required sections and fabricates nothing', () => {
    const raw = freshRaw();
    for (const section of OPTIONAL_SECTIONS) deleteAt(raw, section);

    const normalized = normalizeDeviceStatus(raw);
    expect(Object.keys(normalized).sort()).toEqual([...REQUIRED_SECTIONS].sort());
    for (const section of OPTIONAL_SECTIONS) expect(Object.hasOwn(normalized, section)).toBe(false);
  });

  it.each([
    ['storage_available', 'yes'],
    ['storage_busy', 1],
    ['hardware', 5],
    ['recording', []],
    ['recording_gate', null],
    ['measurement', 'ready'],
    ['object_store', false],
    ['recording_sessions', null],
    ['workflow', []],
    ['ranging', 1],
    ['nrf_logs', {}],
    ['main', []],
  ] as const)('shape-checks a present optional section: %s', (section, value) => {
    const raw = freshRaw();
    setAt(raw, section, value);
    expectInvalid(raw, section);
  });

  it('shape-checks an optional hardware reason code when present', () => {
    const raw = freshRaw();
    setAt(raw, 'hardware.modules.0.reason_code', 5);
    expectInvalid(raw, 'hardware.modules.0.reason_code');
  });

  it('shape-checks object_store.active_id as a string or explicit null', () => {
    const raw = freshRaw();
    setAt(raw, 'object_store.active_id', 5);
    expectInvalid(raw, 'object_store.active_id');
  });

  it.each([
    'pods.0.height_reference',
    'main.height_reference',
    'acquisition.cadence',
    'acquisition.serial',
    'acquisition.signal',
    'ranging.progress',
  ])('shape-checks an optional nested object when present: %s', (path) => {
    const raw = freshRaw();
    setAt(raw, path, []);
    expectInvalid(raw, path);
  });

  it('maps unknown power states without mutating input and preserves other open enum strings', () => {
    const raw = freshRaw();
    setAt(raw, 'power.state', 'SOLAR_BOOST');
    setAt(raw, 'recording.state', 'PAUSED');
    setAt(raw, 'measurement.state', 'FUTURE_MEASUREMENT');
    setAt(raw, 'workflow.step', 'FUTURE_STEP');

    const normalized = normalizeDeviceStatus(raw);
    expect(normalized.power.state).toBe('UNKNOWN');
    expect(normalized.recording?.state).toBe('PAUSED');
    expect(normalized.measurement?.state).toBe('FUTURE_MEASUREMENT');
    expect(normalized.workflow?.step).toBe('FUTURE_STEP');
    expect(valueAt(raw, 'power.state')).toBe('SOLAR_BOOST');
    expect(normalized).not.toBe(raw);
  });

  it.each(['power.voltage_v', 'object_store.active_id', 'main.pressure_pa', 'main.temperature_mc'])(
    'does not fabricate a missing required field: %s',
    (path) => {
      const raw = freshRaw();
      deleteAt(raw, path);

      expectInvalid(raw, path);
      expect(valueAt(raw, path)).toBeUndefined();
    },
  );

  it('is idempotent for full, old-firmware, and normalized open-enum statuses', () => {
    const full = fullDeviceStatus();
    const firstFull = normalizeDeviceStatus(full);
    expect(normalizeDeviceStatus(firstFull)).toEqual(firstFull);

    const oldFirmware = freshRaw();
    for (const section of OPTIONAL_SECTIONS) deleteAt(oldFirmware, section);
    const firstOld = normalizeDeviceStatus(oldFirmware);
    expect(normalizeDeviceStatus(firstOld)).toEqual(firstOld);

    const futureState = freshRaw();
    setAt(futureState, 'power.state', 'SOLAR_BOOST');
    const firstFuture = normalizeDeviceStatus(futureState);
    expect(normalizeDeviceStatus(firstFuture)).toEqual(firstFuture);
  });

  it('reports short diagnostics without echoing a large invalid value', () => {
    const raw = freshRaw();
    const largeValue = 'x'.repeat(10000);
    setAt(raw, 'power.voltage_v', largeValue);

    const error = expectInvalid(raw, 'power.voltage_v');
    expect(error.name).toBe('DeviceStatusValidationError');
    expect(error.reason).toBe('not_finite_number');
    expect(error.message.length).toBeLessThan(160);
    expect(error.message).not.toContain(largeValue);
  });

  it('keeps the complete fixture free of a partial-status type escape', () => {
    const source = readFileSync(new URL('./fixtures/deviceStatusFull.ts', import.meta.url), 'utf8');
    expect(source).not.toContain('as unknown as DeviceStatus');
  });
});
