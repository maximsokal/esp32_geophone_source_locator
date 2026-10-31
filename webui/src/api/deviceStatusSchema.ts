import type { DeviceStatus, DeviceWorkflowStatus } from '../domain/device';

/*
 * Required sections: system, power, environment, mux, pods, acquisition.
 * Optional: storage_available, storage_busy, hardware, recording,
 * recording_gate, measurement, object_store, recording_sessions, workflow,
 * ranging, nrf_logs, main. Present known fields are shape-checked; absent
 * optional fields stay absent. Unknown additive fields are preserved.
 * Unknown power.state strings map to UNKNOWN; other enum-like strings remain
 * open and are preserved. Missing required values throw
 * DeviceStatusValidationError with path and reason.
 * Float fields serialized as null for non-finite values stay null. DeviceStatus
 * types still say number, so UI consumers need finite or valid guards; this
 * validator never fabricates a replacement value.
 */

type StatusObject = Record<string, unknown>;
type Validator = (value: unknown, path: string) => void;

export class DeviceStatusValidationError extends Error {
  readonly path: string;
  readonly reason: string;

  constructor(path: string, reason: string) {
    super(`Invalid device status (${path}: ${reason})`);
    this.name = 'DeviceStatusValidationError';
    this.path = path;
    this.reason = reason;
  }
}

function fail(path: string, reason: string): never {
  throw new DeviceStatusValidationError(path, reason);
}

function isPlainObject(value: unknown): value is StatusObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function objectValue(value: unknown, path: string): StatusObject {
  if (!isPlainObject(value)) fail(path, 'not_object');
  return value;
}

function requiredObject(object: StatusObject, key: string, parentPath: string): StatusObject {
  const path = fieldPath(parentPath, key);
  const value = object[key];
  if (value === undefined) fail(path, 'missing');
  return objectValue(value, path);
}

function fieldPath(parentPath: string, key: string): string {
  return parentPath === '' ? key : `${parentPath}.${key}`;
}

function requiredValue(object: StatusObject, key: string, parentPath: string): unknown {
  const path = fieldPath(parentPath, key);
  const value = object[key];
  if (value === undefined) fail(path, 'missing');
  return value;
}

function finiteNumber(value: unknown, path: string): void {
  if (typeof value !== 'number' || !Number.isFinite(value)) fail(path, 'not_finite_number');
}

function finiteNumberOrNull(value: unknown, path: string): void {
  if (value !== null) finiteNumber(value, path);
}

function booleanValue(value: unknown, path: string): void {
  if (typeof value !== 'boolean') fail(path, 'not_boolean');
}

function booleanOrNull(value: unknown, path: string): void {
  if (value !== null) booleanValue(value, path);
}

function stringValue(value: unknown, path: string): void {
  if (typeof value !== 'string') fail(path, 'not_string');
}

function requiredNumber(object: StatusObject, key: string, parentPath: string): void {
  finiteNumber(requiredValue(object, key, parentPath), fieldPath(parentPath, key));
}

function requiredFloatNumber(object: StatusObject, key: string, parentPath: string): void {
  finiteNumberOrNull(requiredValue(object, key, parentPath), fieldPath(parentPath, key));
}

function requiredBoolean(object: StatusObject, key: string, parentPath: string): void {
  booleanValue(requiredValue(object, key, parentPath), fieldPath(parentPath, key));
}

function requiredString(object: StatusObject, key: string, parentPath: string): void {
  stringValue(requiredValue(object, key, parentPath), fieldPath(parentPath, key));
}

function optionalField(object: StatusObject, key: string, parentPath: string, validator: Validator): void {
  const value = object[key];
  if (value !== undefined) validator(value, fieldPath(parentPath, key));
}

function optionalNumber(object: StatusObject, key: string, parentPath: string): void {
  optionalField(object, key, parentPath, finiteNumber);
}

function optionalAgeMs(object: StatusObject, key: string, parentPath: string): void {
  const value = object[key];
  if (value === undefined) return;
  const path = fieldPath(parentPath, key);
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) fail(path, 'not_nonnegative_integer');
}

function optionalFloatNumber(object: StatusObject, key: string, parentPath: string): void {
  optionalField(object, key, parentPath, finiteNumberOrNull);
}

function optionalBoolean(object: StatusObject, key: string, parentPath: string): void {
  optionalField(object, key, parentPath, booleanValue);
}

function optionalBooleanOrNull(object: StatusObject, key: string, parentPath: string): void {
  optionalField(object, key, parentPath, booleanOrNull);
}

function optionalString(object: StatusObject, key: string, parentPath: string): void {
  optionalField(object, key, parentPath, stringValue);
}

function numberOrNull(value: unknown, path: string): void {
  if (value !== null) finiteNumber(value, path);
}

function stringOrNull(value: unknown, path: string): void {
  if (value !== null) stringValue(value, path);
}

function optionalStringOrNull(object: StatusObject, key: string, parentPath: string): void {
  optionalField(object, key, parentPath, stringOrNull);
}

function requiredNumberOrNull(object: StatusObject, key: string, parentPath: string): void {
  numberOrNull(requiredValue(object, key, parentPath), fieldPath(parentPath, key));
}

function arrayValue(value: unknown, path: string): unknown[] {
  if (!Array.isArray(value)) fail(path, 'not_array');
  return value;
}

function requiredArray(object: StatusObject, key: string, parentPath: string): unknown[] {
  return arrayValue(requiredValue(object, key, parentPath), fieldPath(parentPath, key));
}

function optionalArray(object: StatusObject, key: string, parentPath: string): unknown[] | undefined {
  const value = object[key];
  if (value === undefined) return undefined;
  return arrayValue(value, fieldPath(parentPath, key));
}

function requiredStringArray(object: StatusObject, key: string, parentPath: string): void {
  const path = fieldPath(parentPath, key);
  const values = requiredArray(object, key, parentPath);
  for (let index = 0; index < values.length; index++) stringValue(values[index], `${path}.${index}`);
}

function nullableNumberArray(object: StatusObject, key: string, parentPath: string): void {
  const path = fieldPath(parentPath, key);
  const values = requiredArray(object, key, parentPath);
  for (let index = 0; index < values.length; index++) numberOrNull(values[index], `${path}.${index}`);
}

function nullableFloatNumberArray(object: StatusObject, key: string, parentPath: string): void {
  const path = fieldPath(parentPath, key);
  const values = requiredArray(object, key, parentPath);
  for (let index = 0; index < values.length; index++) finiteNumberOrNull(values[index], `${path}.${index}`);
}

function checkSystem(system: StatusObject): void {
  requiredString(system, 'state', 'system');
  requiredString(system, 'mode', 'system');
  requiredNumber(system, 'uptime_ms', 'system');
  requiredBoolean(system, 'acquisition_ready', 'system');
  requiredNumber(system, 'ready_pods', 'system');
  requiredNumber(system, 'required_pods', 'system');
}

function checkPowerChannel(value: unknown, path: string): void {
  const channel = objectValue(value, path);
  requiredNumber(channel, 'id', path);
  requiredString(channel, 'name', path);
  requiredBoolean(channel, 'valid', path);
  requiredFloatNumber(channel, 'voltage_v', path);
  requiredFloatNumber(channel, 'current_a', path);
  requiredFloatNumber(channel, 'power_w', path);
  optionalString(channel, 'overcurrent', path);
  optionalBoolean(channel, 'overcurrent_latched', path);
  optionalAgeMs(channel, 'age_ms', path);
  optionalBoolean(channel, 'stale', path);
}

function checkPower(power: StatusObject): void {
  requiredBoolean(power, 'valid', 'power');
  requiredFloatNumber(power, 'voltage_v', 'power');
  requiredFloatNumber(power, 'current_a', 'power');
  requiredFloatNumber(power, 'power_w', 'power');
  optionalBoolean(power, 'battery_present', 'power');
  optionalBoolean(power, 'battery_percent_valid', 'power');
  optionalFloatNumber(power, 'battery_percent_estimate', 'power');
  optionalFloatNumber(power, 'battery_voltage_v', 'power');
  optionalFloatNumber(power, 'battery_current_a', 'power');
  optionalBoolean(power, 'usb_present', 'power');
  optionalBoolean(power, 'charging', 'power');
  optionalBoolean(power, 'battery_full', 'power');
  optionalString(power, 'low_battery', 'power');
  optionalNumber(power, 'shutdown_in_s', 'power');
  optionalString(power, 'battery_soc_source', 'power');
  optionalString(power, 'state', 'power');
  optionalAgeMs(power, 'age_ms', 'power');
  optionalBoolean(power, 'stale', 'power');
  const channels = optionalArray(power, 'channels', 'power');
  if (channels !== undefined) {
    for (let index = 0; index < channels.length; index++) {
      checkPowerChannel(channels[index], `power.channels.${index}`);
    }
  }
}

function checkTopology(value: unknown): void {
  const topology = objectValue(value, 'topology');
  requiredBoolean(topology, 'valid', 'topology');
  requiredNumber(topology, 'age_ms', 'topology');
  optionalAgeMs(topology, 'age_ms', 'topology');
  requiredBoolean(topology, 'stale', 'topology');
}

function checkNodeLinks(value: unknown): void {
  const links = arrayValue(value, 'node_links');
  for (let index = 0; index < links.length; index++) {
    const path = `node_links.${index}`;
    const link = objectValue(links[index], path);
    requiredNumber(link, 'channel', path);
    requiredString(link, 'node', path);
    requiredString(link, 'status', path);
  }
}

function checkPod(podValue: unknown, path: string): void {
  const pod = objectValue(podValue, path);
  requiredNumber(pod, 'id', path);
  requiredString(pod, 'state', path);
  optionalString(pod, 'node_link_status', path);

  const capabilitiesValue = pod.capabilities;
  if (capabilitiesValue !== undefined) {
    const capabilities = objectValue(capabilitiesValue, `${path}.capabilities`);
    for (const key of ['barometer', 'environment', 'imu', 'geophone', 'local_position', 'global_position']) {
      requiredBoolean(capabilities, key, `${path}.capabilities`);
    }
  }

  const barometer = requiredObject(pod, 'barometer', path);
  requiredBoolean(barometer, 'valid', `${path}.barometer`);
  requiredFloatNumber(barometer, 'pressure_pa', `${path}.barometer`);
  requiredFloatNumber(barometer, 'relative_altitude_cm', `${path}.barometer`);
  requiredFloatNumber(barometer, 'reference_pressure_pa', `${path}.barometer`);

  const environment = requiredObject(pod, 'environment', path);
  requiredBoolean(environment, 'valid', `${path}.environment`);
  optionalBoolean(environment, 'temperature_valid', `${path}.environment`);
  requiredFloatNumber(environment, 'temperature_c', `${path}.environment`);
  optionalBoolean(environment, 'humidity_valid', `${path}.environment`);
  requiredFloatNumber(environment, 'humidity_percent', `${path}.environment`);

  const imu = requiredObject(pod, 'imu', path);
  requiredBoolean(imu, 'valid', `${path}.imu`);
  requiredFloatNumber(imu, 'roll_deg', `${path}.imu`);
  requiredFloatNumber(imu, 'pitch_deg', `${path}.imu`);
  requiredFloatNumber(imu, 'yaw_relative_deg', `${path}.imu`);

  if (pod.sensor_health !== undefined) {
    const health = objectValue(pod.sensor_health, `${path}.sensor_health`);
    for (const key of ['current', 'imu_valid', 'barometer_valid', 'imu_fault', 'barometer_fault', 'imu_calibrated']) {
      requiredBoolean(health, key, `${path}.sensor_health`);
    }
  }

  const geophone = requiredObject(pod, 'geophone', path);
  requiredBoolean(geophone, 'valid', `${path}.geophone`);
  requiredFloatNumber(geophone, 'instant_uv', `${path}.geophone`);
  requiredFloatNumber(geophone, 'rms_uv', `${path}.geophone`);
  requiredFloatNumber(geophone, 'peak_uv', `${path}.geophone`);
  requiredFloatNumber(geophone, 'noise_rms_uv', `${path}.geophone`);
  requiredFloatNumber(geophone, 'snr_db', `${path}.geophone`);

  const position = requiredObject(pod, 'position', path);
  requiredBoolean(position, 'local_valid', `${path}.position`);
  requiredFloatNumber(position, 'local_x_m', `${path}.position`);
  requiredFloatNumber(position, 'local_y_m', `${path}.position`);
  requiredBoolean(position, 'global_valid', `${path}.position`);
  requiredFloatNumber(position, 'latitude', `${path}.position`);
  requiredFloatNumber(position, 'longitude', `${path}.position`);
  requiredBoolean(position, 'altitude_valid', `${path}.position`);
  requiredFloatNumber(position, 'altitude_m', `${path}.position`);

  const heightReference = pod.height_reference;
  if (heightReference !== undefined) {
    const height = objectValue(heightReference, `${path}.height_reference`);
    requiredString(height, 'state', `${path}.height_reference`);
    requiredNumber(height, 'generation', `${path}.height_reference`);
    requiredNumber(height, 'relative_z_mm', `${path}.height_reference`);
  }
}

function checkPods(value: unknown): void {
  const pods = arrayValue(value, 'pods');
  for (let index = 0; index < pods.length; index++) checkPod(pods[index], `pods.${index}`);
}

function checkEnvironment(environment: StatusObject): void {
  requiredBoolean(environment, 'valid', 'environment');
  requiredFloatNumber(environment, 'temperature_c', 'environment');
  requiredFloatNumber(environment, 'humidity_percent', 'environment');
  requiredFloatNumber(environment, 'pressure_pa', 'environment');
}

function checkMux(mux: StatusObject): void {
  requiredBoolean(mux, 'valid', 'mux');
  requiredNumber(mux, 'active_channel', 'mux');
  requiredNumber(mux, 'active_pod', 'mux');
}

function checkAcquisition(acquisition: StatusObject): void {
  requiredBoolean(acquisition, 'running', 'acquisition');
  requiredBoolean(acquisition, 'requested_running', 'acquisition');
  requiredBoolean(acquisition, 'hardware_ready', 'acquisition');
  requiredString(acquisition, 'source', 'acquisition');
  optionalNumber(acquisition, 'last_error', 'acquisition');
  optionalString(acquisition, 'last_error_name', 'acquisition');
  requiredNumber(acquisition, 'sample_rate_hz', 'acquisition');
  requiredNumber(acquisition, 'samples_recorded', 'acquisition');
  requiredNumber(acquisition, 'recording_ms', 'acquisition');

  const cadenceValue = acquisition.cadence;
  if (cadenceValue !== undefined) {
    const cadence = objectValue(cadenceValue, 'acquisition.cadence');
    requiredBoolean(cadence, 'available', 'acquisition.cadence');
    requiredNumber(cadence, 'timer_ticks', 'acquisition.cadence');
    requiredNumber(cadence, 'conversion_attempts', 'acquisition.cadence');
    requiredNumber(cadence, 'collapsed_periods', 'acquisition.cadence');
    requiredNumber(cadence, 'last_successful_sample_interval_us', 'acquisition.cadence');
    requiredNumber(cadence, 'max_successful_sample_interval_us', 'acquisition.cadence');
  }

  const serialValue = acquisition.serial;
  if (serialValue !== undefined) {
    const serial = objectValue(serialValue, 'acquisition.serial');
    requiredBoolean(serial, 'suspicious', 'acquisition.serial');
    requiredString(serial, 'pattern', 'acquisition.serial');
    optionalString(serial, 'quality', 'acquisition.serial');
    optionalString(serial, 'stream_state', 'acquisition.serial');
    optionalBoolean(serial, 'stream_stuck', 'acquisition.serial');
    optionalString(serial, 'power_gate', 'acquisition.serial');
    optionalBoolean(serial, 'power_fault_latched', 'acquisition.serial');
    optionalFloatNumber(serial, 'power_fault_rail_v', 'acquisition.serial');
    optionalFloatNumber(serial, 'power_fault_current_a', 'acquisition.serial');
    requiredNumber(serial, 'suspicious_streak', 'acquisition.serial');
    optionalNumber(serial, 'repeated_frame_streak', 'acquisition.serial');
    optionalNumber(serial, 'all_zero_frames', 'acquisition.serial');
    optionalNumber(serial, 'all_ff_frames', 'acquisition.serial');
    optionalNumber(serial, 'all_identical_frames', 'acquisition.serial');
    optionalNumber(serial, 'varied_frames', 'acquisition.serial');
    optionalNumber(serial, 'digital_interference_frames', 'acquisition.serial');
    optionalNumber(serial, 'quality_window_frames', 'acquisition.serial');
    optionalFloatNumber(serial, 'quality_bad_ratio', 'acquisition.serial');
    optionalFloatNumber(serial, 'quality_all_00_ratio', 'acquisition.serial');
    optionalFloatNumber(serial, 'quality_all_ff_ratio', 'acquisition.serial');
    optionalFloatNumber(serial, 'quality_all_identical_ratio', 'acquisition.serial');
    optionalFloatNumber(serial, 'quality_repeated_ratio', 'acquisition.serial');
    optionalFloatNumber(serial, 'quality_digital_interference_ratio', 'acquisition.serial');
    requiredNumber(serial, 'busy_level', 'acquisition.serial');
  }

  const signalValue = acquisition.signal;
  if (signalValue !== undefined) {
    const signal = objectValue(signalValue, 'acquisition.signal');
    optionalString(signal, 'quality', 'acquisition.signal');
    optionalNumber(signal, 'channel_count', 'acquisition.signal');
    optionalNumber(signal, 'rail_hit_frames', 'acquisition.signal');
    optionalNumber(signal, 'clip_frames', 'acquisition.signal');
    optionalNumber(signal, 'quality_window_frames', 'acquisition.signal');
    optionalFloatNumber(signal, 'rail_hit_ratio', 'acquisition.signal');
    optionalFloatNumber(signal, 'clip_ratio', 'acquisition.signal');
  }
}

function checkMain(main: StatusObject): void {
  requiredNumber(main, 'pressure_pa', 'main');
  requiredNumber(main, 'temperature_mc', 'main');
  const heightValue = main.height_reference;
  if (heightValue !== undefined) {
    const height = objectValue(heightValue, 'main.height_reference');
    requiredString(height, 'state', 'main.height_reference');
    requiredNumber(height, 'generation', 'main.height_reference');
    requiredNumber(height, 'relative_z_mm', 'main.height_reference');
  }
  for (const key of ['sensor_present', 'sensor_valid']) {
    const sensorsValue = main[key];
    if (sensorsValue !== undefined) {
      const sensors = objectValue(sensorsValue, `main.${key}`);
      requiredBoolean(sensors, 'bmp280', `main.${key}`);
    }
  }
}

function checkHardware(hardware: StatusObject): void {
  requiredBoolean(hardware, 'required_missing', 'hardware');
  const modules = requiredArray(hardware, 'modules', 'hardware');
  for (let index = 0; index < modules.length; index++) {
    const path = `hardware.modules.${index}`;
    const module = objectValue(modules[index], path);
    requiredString(module, 'id', path);
    requiredString(module, 'state', path);
    requiredBoolean(module, 'required', path);
    requiredString(module, 'reason', path);
    optionalString(module, 'reason_code', path);
  }
}

function checkRecording(recording: StatusObject): void {
  requiredBoolean(recording, 'available', 'recording');
  optionalBoolean(recording, 'running', 'recording');
  optionalString(recording, 'state', 'recording');
  for (const key of ['stop_stage', 'stop_error_stage', 'object_id', 'session_id', 'source', 'last_error_name', 'error']) {
    optionalString(recording, key, 'recording');
  }
  for (const key of ['stop_stage_error', 'sample_rate_hz', 'channel_count', 'samples_written', 'dropped_frames', 'recording_ms', 'last_error']) {
    optionalNumber(recording, key, 'recording');
  }
}

function checkRecordingGate(gate: StatusObject): void {
  requiredBoolean(gate, 'available', 'recording_gate');
  for (const key of ['assigned_count', 'accepted_count']) optionalNumber(gate, key, 'recording_gate');
  for (const key of ['decision', 'dimensionality', 'reason', 'warning', 'error']) optionalString(gate, key, 'recording_gate');
  optionalBoolean(gate, 'allowed', 'recording_gate');
  optionalBoolean(gate, 'bypassed', 'recording_gate');
  const nodes = optionalArray(gate, 'nodes', 'recording_gate');
  if (nodes !== undefined) {
    for (let index = 0; index < nodes.length; index++) {
      const path = `recording_gate.nodes.${index}`;
      const node = objectValue(nodes[index], path);
      requiredNumber(node, 'channel', path);
      requiredNumber(node, 'node_id', path);
      requiredString(node, 'state', path);
      optionalFloatNumber(node, 'tilt_deg', path);
    }
  }
}

function checkMeasurement(measurement: StatusObject): void {
  requiredBoolean(measurement, 'available', 'measurement');
  for (const key of ['initialized', 'busy', 'active_object_present']) optionalBoolean(measurement, key, 'measurement');
  for (const key of ['state', 'object_id', 'object_name', 'session_id', 'stop_stage', 'stop_error_stage', 'last_error_name', 'error']) {
    optionalString(measurement, key, 'measurement');
  }
  for (const key of ['stop_stage_error', 'samples_written', 'dropped_frames', 'recording_ms', 'last_error']) {
    optionalNumber(measurement, key, 'measurement');
  }
}

function checkObjectStore(store: StatusObject): void {
  requiredBoolean(store, 'available', 'object_store');
  stringOrNull(requiredValue(store, 'active_id', 'object_store'), 'object_store.active_id');
  nullableFloatNumberArray(store, 'default_cable_lengths_m', 'object_store');
  const objects = requiredArray(store, 'objects', 'object_store');
  for (let index = 0; index < objects.length; index++) {
    const path = `object_store.objects.${index}`;
    const profile = objectValue(objects[index], path);
    requiredString(profile, 'id', path);
    requiredString(profile, 'name', path);
    requiredBoolean(profile, 'active', path);
    nullableFloatNumberArray(profile, 'cable_lengths_m', path);
    if (profile.delete_scope !== undefined) {
      const scope = objectValue(profile.delete_scope, `${path}.delete_scope`);
      requiredBoolean(scope, 'available', `${path}.delete_scope`);
      requiredNumber(scope, 'sessions', `${path}.delete_scope`);
      requiredNumber(scope, 'artifacts', `${path}.delete_scope`);
      requiredNumber(scope, 'localization_runs', `${path}.delete_scope`);
    }
    const location = requiredObject(profile, 'location', path);
    requiredBoolean(location, 'valid', `${path}.location`);
    requiredFloatNumber(location, 'latitude', `${path}.location`);
    requiredFloatNumber(location, 'longitude', `${path}.location`);
    requiredFloatNumber(location, 'accuracy_m', `${path}.location`);
    requiredBoolean(location, 'altitude_valid', `${path}.location`);
    requiredFloatNumber(location, 'altitude_m', `${path}.location`);
    requiredFloatNumber(location, 'altitude_accuracy_m', `${path}.location`);
    requiredNumber(location, 'captured_at_ms', `${path}.location`);
    requiredString(location, 'source', `${path}.location`);
  }
  optionalString(store, 'error', 'object_store');
}

function checkRecordingSessions(sessions: StatusObject): void {
  requiredString(sessions, 'object_id', 'recording_sessions');
  requiredBoolean(sessions, 'available', 'recording_sessions');
  optionalString(sessions, 'error', 'recording_sessions');
  const items = requiredArray(sessions, 'sessions', 'recording_sessions');
  for (let index = 0; index < items.length; index++) {
    const path = `recording_sessions.sessions.${index}`;
    const item = objectValue(items[index], path);
    requiredString(item, 'session_id', path);
    optionalString(item, 'status', path);
    requiredBoolean(item, 'complete', path);
    optionalBoolean(item, 'readable', path);
    optionalBoolean(item, 'recovered', path);
    optionalString(item, 'source', path);
    requiredNumber(item, 'sample_rate_hz', path);
    requiredNumber(item, 'channel_count', path);
    requiredNumber(item, 'samples_written', path);
    requiredNumber(item, 'dropped_frames', path);
    optionalBoolean(item, 'dropped_frames_available', path);
    requiredNumber(item, 'recording_ms', path);
    optionalBoolean(item, 'recording_ms_available', path);
  }
}

function checkWorkflow(workflow: StatusObject): void {
  requiredString(workflow, 'step', 'workflow');
  requiredNumber(workflow, 'generation', 'workflow');
  if (!Number.isInteger(workflow.generation) || (workflow.generation as number) < 0 ||
      (workflow.generation as number) > 0xffffffff) fail('workflow.generation', 'not_u32');
  requiredStringArray(workflow, 'allowed_actions', 'workflow');
  optionalString(workflow, 'sensor_sufficiency', 'workflow');
  optionalNumber(workflow, 'assigned_remotes', 'workflow');
  optionalStringOrNull(workflow, 'reason', 'workflow');
  requiredNumberOrNull(workflow, 'progress', 'workflow');
  optionalStringOrNull(workflow, 'object_id', 'workflow');
  optionalStringOrNull(workflow, 'session_id', 'workflow');
  optionalBoolean(workflow, 'has_scan', 'workflow');
  optionalBoolean(workflow, 'calc_settings_ready', 'workflow');
  optionalString(workflow, 'calculation_class', 'workflow');
  optionalString(workflow, 'gps_status', 'workflow');
  optionalBooleanOrNull(workflow, 'layout_result', 'workflow');
  optionalStringOrNull(workflow, 'layout_reason', 'workflow');
  optionalString(workflow, 'scan_choice', 'workflow');
  optionalString(workflow, 'scan_upload_status', 'workflow');

  const calibrationValue = workflow.calibration;
  if (calibrationValue === undefined && workflow.step === 'CALIB_RESULT') {
    fail('workflow.calibration', 'missing');
  }
  if (calibrationValue !== undefined) {
    const calibration = objectValue(calibrationValue, 'workflow.calibration');
    requiredNumber(calibration, 'generation', 'workflow.calibration');
    requiredString(calibration, 'phase', 'workflow.calibration');
    requiredBooleanOrNull(calibration, 'result', 'workflow.calibration');
    requiredStringArray(calibration, 'reasons', 'workflow.calibration');
    const calibrationNodes = requiredArray(calibration, 'per_node', 'workflow.calibration');
    for (let index = 0; index < calibrationNodes.length; index++) {
      const path = `workflow.calibration.per_node.${index}`;
      const node = objectValue(calibrationNodes[index], path);
      requiredString(node, 'node', path);
      const height = requiredObject(node, 'height', path);
      requiredString(height, 'state', `${path}.height`);
      requiredNumberOrNull(height, 'relative_z_mm', `${path}.height`);
      const imu = requiredObject(node, 'imu', path);
      requiredString(imu, 'state', `${path}.imu`);
      nullableNumberArray(imu, 'gyro_bias_mdps', `${path}.imu`);
      nullableNumberArray(imu, 'gravity_ref_mg', `${path}.imu`);
      requiredBoolean(node, 'cs_pairs_ok', path);
    }
    const cs = requiredObject(calibration, 'cs', 'workflow.calibration');
    requiredNumber(cs, 'pairs_done', 'workflow.calibration.cs');
    requiredNumber(cs, 'pairs_total', 'workflow.calibration.cs');
    requiredNumberOrNull(cs, 'max_spread_mm', 'workflow.calibration.cs');
  }

  const placementValue = workflow.placement;
  if (placementValue === undefined && workflow.step === 'PLACEMENT') {
    fail('workflow.placement', 'missing');
  }
  if (placementValue !== undefined) {
    const placement = objectValue(placementValue, 'workflow.placement');
    requiredBoolean(placement, 'all_stable', 'workflow.placement');
    requiredNumber(placement, 'max_range_mm', 'workflow.placement');
    requiredNumber(placement, 'min_samples', 'workflow.placement');
    const placementNodes = requiredArray(placement, 'per_node', 'workflow.placement');
    for (let index = 0; index < placementNodes.length; index++) {
      const path = `workflow.placement.per_node.${index}`;
      const node = objectValue(placementNodes[index], path);
      requiredString(node, 'node', path);
      requiredBoolean(node, 'vertical', path);
      requiredNumberOrNull(node, 'height_rel_main_mm', path);
      finiteNumberOrNull(requiredValue(node, 'altitude_asl_m', path), `${path}.altitude_asl_m`);
      requiredBoolean(node, 'stable', path);
      requiredString(node, 'reason', path);
      requiredNumber(node, 'samples', path);
      requiredNumberOrNull(node, 'range_mm', path);
    }
  }
}

function requiredBooleanOrNull(object: StatusObject, key: string, parentPath: string): void {
  const path = fieldPath(parentPath, key);
  const value = requiredValue(object, key, parentPath);
  if (value !== null) booleanValue(value, path);
}

function checkRangingPairs(value: unknown, path: string): void {
  const pairs = arrayValue(value, path);
  for (let index = 0; index < pairs.length; index++) {
    const itemPath = `${path}.${index}`;
    const pair = objectValue(pairs[index], itemPath);
    requiredNumber(pair, 'node_a', itemPath);
    requiredNumber(pair, 'node_b', itemPath);
    requiredString(pair, 'state', itemPath);
    requiredNumber(pair, 'distance_mm', itemPath);
    requiredNumber(pair, 'spread_mm', itemPath);
    requiredNumber(pair, 'confidence', itemPath);
    requiredNumber(pair, 'valid_samples', itemPath);
    requiredNumber(pair, 'attempts', itemPath);
    requiredBoolean(pair, 'single_direction', itemPath);
    requiredNumber(pair, 'direction_disagreement_mm', itemPath);
    if (pair.manual !== undefined) booleanValue(pair.manual, `${itemPath}.manual`);
    optionalNumber(pair, 'automatic_distance_mm', itemPath);
  }
}

function checkRangingGeometry(value: unknown, path: string): void {
  const geometry = objectValue(value, path);
  requiredNumber(geometry, 'residual_mm', path);
  requiredNumber(geometry, 'max_residual_mm', path);
  const nodes = requiredArray(geometry, 'nodes', path);
  for (let index = 0; index < nodes.length; index++) {
    const nodePath = `${path}.nodes.${index}`;
    const node = objectValue(nodes[index], nodePath);
    requiredNumber(node, 'node_id', nodePath);
    requiredBoolean(node, 'solved', nodePath);
    requiredNumber(node, 'x_mm', nodePath);
    requiredNumber(node, 'y_mm', nodePath);
    requiredNumber(node, 'z_mm', nodePath);
    requiredNumber(node, 'node_residual_mm', nodePath);
    requiredNumber(node, 'uncertainty_mm', nodePath);
  }
}

function checkRangingProgress(progress: StatusObject): void {
  requiredBoolean(progress, 'active', 'ranging.progress');
  requiredBoolean(progress, 'complete', 'ranging.progress');
  requiredBoolean(progress, 'success', 'ranging.progress');
  requiredNumber(progress, 'pairs_done', 'ranging.progress');
  requiredNumber(progress, 'pairs_total', 'ranging.progress');
  requiredNumber(progress, 'solution_residual_mm', 'ranging.progress');
  for (const key of ['topology_generation', 'membership_generation', 'main_height_reference_generation', 'started_at_ms', 'updated_at_ms']) {
    requiredNumber(progress, key, 'ranging.progress');
  }
  requiredString(progress, 'failure_reason', 'ranging.progress');
  checkRangingPairs(requiredValue(progress, 'pairs', 'ranging.progress'), 'ranging.progress.pairs');
  checkRangingGeometry(requiredValue(progress, 'geometry', 'ranging.progress'), 'ranging.progress.geometry');
}

function checkRangingResult(result: StatusObject): void {
  requiredBoolean(result, 'available', 'ranging.last_result');
  requiredBoolean(result, 'invalidated', 'ranging.last_result');
  if (result.available === true) {
    for (const key of ['topology_generation', 'membership_generation', 'main_height_reference_generation', 'timestamp_ms']) {
      requiredNumber(result, key, 'ranging.last_result');
    }
    checkRangingPairs(requiredValue(result, 'pairs', 'ranging.last_result'), 'ranging.last_result.pairs');
    checkRangingGeometry(requiredValue(result, 'geometry', 'ranging.last_result'), 'ranging.last_result.geometry');
    return;
  }

  for (const key of ['topology_generation', 'membership_generation', 'main_height_reference_generation', 'timestamp_ms']) {
    optionalNumber(result, key, 'ranging.last_result');
  }
  const pairs = optionalArray(result, 'pairs', 'ranging.last_result');
  if (pairs !== undefined) checkRangingPairs(pairs, 'ranging.last_result.pairs');
  const geometry = result.geometry;
  if (geometry !== undefined) checkRangingGeometry(geometry, 'ranging.last_result.geometry');
}

function checkRanging(ranging: StatusObject): void {
  requiredBoolean(ranging, 'available', 'ranging');
  const progressValue = ranging.progress;
  if (progressValue === undefined && ranging.available === true) {
    fail('ranging.progress', 'missing');
  }
  if (progressValue !== undefined) {
    checkRangingProgress(objectValue(progressValue, 'ranging.progress'));
  }
  checkRangingResult(requiredObject(ranging, 'last_result', 'ranging'));
}

function checkNrfLogs(value: unknown): void {
  const nodes = arrayValue(value, 'nrf_logs');
  for (let index = 0; index < nodes.length; index++) {
    const path = `nrf_logs.${index}`;
    const node = objectValue(nodes[index], path);
    requiredString(node, 'node', path);
    requiredNumberOrNull(node, 'channel', path);
    requiredBoolean(node, 'present', path);
    requiredBoolean(node, 'logs_unsupported', path);
    requiredNumberOrNull(node, 'boot_id', path);
    for (const key of ['stored', 'warn_count', 'error_count', 'fatal_count', 'wire_gap_total', 'local_drop_total', 'overwritten_total']) {
      requiredNumber(node, key, path);
    }
  }
}

/** Apply the same runtime contract to command responses and status polling. */
export function normalizeWorkflowStatus(raw: unknown): DeviceWorkflowStatus {
  const workflow = objectValue(raw, 'workflow');
  checkWorkflow(workflow);
  return workflow as unknown as DeviceWorkflowStatus;
}

/** Validate /api/status in place, preserving unknown fields and object identity when possible. */
export function normalizeDeviceStatus(raw: unknown): DeviceStatus {
  const root = objectValue(raw, '$');

  if (root.storage_available !== undefined) booleanValue(root.storage_available, 'storage_available');
  if (root.storage_busy !== undefined) booleanValue(root.storage_busy, 'storage_busy');

  checkSystem(requiredObject(root, 'system', ''));
  const power = requiredObject(root, 'power', '');
  checkPower(power);
  checkEnvironment(requiredObject(root, 'environment', ''));
  checkMux(requiredObject(root, 'mux', ''));
  checkPods(requiredValue(root, 'pods', ''));
  checkAcquisition(requiredObject(root, 'acquisition', ''));
  if (root.topology !== undefined) checkTopology(root.topology);
  if (root.node_links !== undefined) checkNodeLinks(root.node_links);

  if (root.main !== undefined) checkMain(objectValue(root.main, 'main'));
  if (root.hardware !== undefined) checkHardware(objectValue(root.hardware, 'hardware'));
  if (root.recording !== undefined) checkRecording(objectValue(root.recording, 'recording'));
  if (root.recording_gate !== undefined) checkRecordingGate(objectValue(root.recording_gate, 'recording_gate'));
  if (root.measurement !== undefined) checkMeasurement(objectValue(root.measurement, 'measurement'));
  if (root.object_store !== undefined) checkObjectStore(objectValue(root.object_store, 'object_store'));
  if (root.recording_sessions !== undefined) checkRecordingSessions(objectValue(root.recording_sessions, 'recording_sessions'));
  if (root.workflow !== undefined) checkWorkflow(objectValue(root.workflow, 'workflow'));
  if (root.ranging !== undefined) checkRanging(objectValue(root.ranging, 'ranging'));
  if (root.nrf_logs !== undefined) checkNrfLogs(root.nrf_logs);

  if (power.state !== undefined) {
    const state = power.state;
    if (typeof state !== 'string') fail('power.state', 'not_string');
    switch (state) {
      case 'USB_ONLY_NO_BATTERY':
      case 'USB_CHARGING':
      case 'USB_BATTERY_READY':
      case 'BATTERY':
      case 'BATTERY_LOW':
      case 'BATTERY_CRITICAL':
      case 'UNKNOWN':
        return root as unknown as DeviceStatus;
      default:
        return { ...root, power: { ...power, state: 'UNKNOWN' } } as unknown as DeviceStatus;
    }
  }

  return root as unknown as DeviceStatus;
}
