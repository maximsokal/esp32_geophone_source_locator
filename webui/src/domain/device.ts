export interface ObjectLocation {
  valid: boolean;
  latitude: number;
  longitude: number;
  accuracy_m: number;
  altitude_valid: boolean;
  altitude_m: number;
  altitude_accuracy_m: number;
  captured_at_ms: number;
  source: string;
}

export interface ObjectDeleteScope {
  available: boolean;
  sessions: number;
  artifacts: number;
  localization_runs: number;
}

export interface ObjectProfile {
  id: string;
  name: string;
  active: boolean;
  location: ObjectLocation;
  cable_lengths_m: number[];
  delete_scope?: ObjectDeleteScope;
}

export interface ObjectStoreState {
  available: boolean;
  active_id: string | null;
  default_cable_lengths_m: number[];
  objects: ObjectProfile[];
  error?: string;
}

export interface RecordingState {
  available: boolean;
  running?: boolean;
  state?: 'IDLE' | 'RECORDING' | 'STOPPING' | 'ERROR';
  stop_stage?: string;
  stop_error_stage?: string;
  stop_stage_error?: number;
  object_id?: string;
  session_id?: string;
  source?: string;
  sample_rate_hz?: number;
  channel_count?: number;
  samples_written?: number;
  dropped_frames?: number;
  recording_ms?: number;
  last_error?: number;
  last_error_name?: string;
  error?: string;
}

export type RecordingGateDecision =
  | 'BLOCKED_INSUFFICIENT_REMOTES'
  | 'BLOCKED_NOT_LEVEL'
  | 'ALLOWED_PLANAR_2D'
  | 'ALLOWED_3D_CAPABLE';

export type LocalizationDimensionality =
  | 'INSUFFICIENT'
  | 'PLANAR_2D'
  | 'CAPABLE_3D';

/**
 * MAX-165: the same Record-button admission decision seismic_recorder_start()
 * itself enforces, surfaced ahead of time so a rejection can be shown before
 * the operator presses Record rather than only after.
 */
export interface RecordingGateStatus {
  available: boolean;
  assigned_count?: number;
  accepted_count?: number;
  decision?: RecordingGateDecision;
  dimensionality?: LocalizationDimensionality;
  allowed?: boolean;
  bypassed?: boolean;
  reason?: string;
  warning?: string;
  error?: string;
  /** MAX-114: every assigned REMOTE; tilt_deg only when the node reported one. */
  nodes?: RecordingGateNode[];
}

export interface RecordingGateNode {
  channel: number;
  node_id: number;
  /** level | tilted | moving | unavailable | no_data */
  state: string;
  tilt_deg?: number;
}

export type MeasurementServiceState =
  | 'LIVE'
  | 'STARTING'
  | 'RECORDING'
  | 'STOPPING'
  | 'REVIEW'
  | 'DELETING'
  | 'ERROR';

export interface MeasurementServiceStatus {
  available: boolean;
  initialized?: boolean;
  busy?: boolean;
  active_object_present?: boolean;
  state?: MeasurementServiceState | string;
  object_id?: string;
  object_name?: string;
  session_id?: string;
  stop_stage?: string;
  stop_error_stage?: string;
  stop_stage_error?: number;
  samples_written?: number;
  dropped_frames?: number;
  recording_ms?: number;
  last_error?: number;
  last_error_name?: string;
  error?: string;
}

export type WorkflowStep =
  | 'IDLE'
  | 'OBJECT'
  | 'SENSOR_CHECK'
  | 'CALIB_PREP'
  | 'CALIBRATING'
  | 'CALIB_RESULT'
  | 'PLACEMENT'
  | 'GPS_CHOICE'
  | 'GPS_PENDING'
  | 'LAYOUT_READY'
  | 'RANGING'
  | 'RANGING_RESULT'
  | 'SCAN_CHOICE'
  | 'SCANNING'
  | 'RECORD_READY'
  | 'RECORDING'
  | 'STOPPED'
  | 'CALCULATING'
  | 'RESULT';

export type WorkflowSensorSufficiency = 'INSUFFICIENT' | 'PLANAR_2D' | 'CAPABLE_3D';

export interface WorkflowCalibrationNode {
  node: string;
  height: { state: string; relative_z_mm: number | null };
  imu: {
    state: string;
    gyro_bias_mdps: (number | null)[];
    gravity_ref_mg: (number | null)[];
  };
  cs_pairs_ok: boolean;
}

export interface WorkflowCalibrationStatus {
  generation: number;
  phase: string;
  per_node: WorkflowCalibrationNode[];
  cs: { pairs_done: number; pairs_total: number; max_spread_mm: number | null };
  result: boolean | null;
  reasons: string[];
}

export interface WorkflowPlacementNode {
  node: string;
  vertical: boolean;
  height_rel_main_mm: number | null;
  altitude_asl_m: number | null;
  stable: boolean;
  /** Why the node is (not) stable: a workflow_placement_reason_t name from the firmware. */
  reason: string;
  /** Samples currently inside the stability window. */
  samples: number;
  /** Height spread of those samples; null until the minimum number of samples is collected. */
  range_mm: number | null;
}

export interface WorkflowPlacementStatus {
  per_node: WorkflowPlacementNode[];
  all_stable: boolean;
  max_range_mm: number;
  min_samples: number;
}

export interface DeviceWorkflowStatus {
  step: WorkflowStep | (string & {});
  generation: number;
  allowed_actions: string[];
  sensor_sufficiency?: WorkflowSensorSufficiency;
  assigned_remotes?: number | null;
  reason?: string | null;
  progress: number | null;
  calibration: WorkflowCalibrationStatus;
  placement: WorkflowPlacementStatus;
  object_id?: string | null;
  session_id?: string | null;
  has_scan?: boolean;
  calc_settings_ready?: boolean;
  calculation_class?: string | null;
  gps_status?: string;
  layout_result?: boolean | null;
  layout_reason?: string | null;
  scan_choice?: string;
  scan_upload_status?: string;
}

export interface RangingPairStatus {
  node_a: number;
  node_b: number;
  state: string;
  distance_mm: number;
  spread_mm: number;
  confidence: number;
  valid_samples: number;
  attempts: number;
  single_direction: boolean;
  direction_disagreement_mm: number;
  manual?: boolean;
  automatic_distance_mm?: number;
}

export interface RangingNodeStatus {
  node_id: number;
  solved: boolean;
  x_mm: number;
  y_mm: number;
  z_mm: number;
  node_residual_mm: number;
  uncertainty_mm: number;
}

export interface RangingGeometryStatus {
  residual_mm: number;
  max_residual_mm: number;
  nodes: RangingNodeStatus[];
}

export interface RangingProgressStatus {
  active?: boolean;
  complete?: boolean;
  success?: boolean;
  partial?: boolean;
  pairs_done?: number | null;
  pairs_total?: number | null;
  topology_generation?: number;
  membership_generation?: number;
  main_height_reference_generation?: number;
  started_at_ms?: number;
  updated_at_ms?: number;
  failure_reason?: string | null;
  solution_residual_mm?: number | null;
  pairs?: RangingPairStatus[];
  geometry?: RangingGeometryStatus;
}

export interface RangingLastResultStatus {
  available: boolean;
  invalidated?: boolean;
  topology_generation?: number;
  membership_generation?: number;
  main_height_reference_generation?: number;
  timestamp_ms?: number;
  pairs?: RangingPairStatus[];
  geometry?: RangingGeometryStatus;
}

export interface DeviceRangingStatus {
  available: boolean;
  progress?: RangingProgressStatus;
  last_result?: RangingLastResultStatus;
}

export type RecordingSessionStatus =
  | 'complete'
  | 'incomplete'
  | 'error'
  | 'recovered_error'
  | 'unknown'
  | (string & {});

export interface RecordingSessionSummary {
  session_id: string;
  status?: RecordingSessionStatus;
  complete: boolean;
  readable?: boolean;
  recovered?: boolean;
  source?: string;
  sample_rate_hz: number;
  channel_count: number;
  samples_written: number;
  dropped_frames: number;
  dropped_frames_available?: boolean;
  recording_ms: number;
  recording_ms_available?: boolean;
}

export interface RecordingSessionCollection {
  object_id: string;
  available: boolean;
  sessions: RecordingSessionSummary[];
  error?: string;
}

export interface ValidValue {
  valid: boolean;
}

export interface PodBarometer extends ValidValue {
  pressure_pa: number;
  relative_altitude_cm: number;
  reference_pressure_pa: number;
}

export interface PodImu extends ValidValue {
  roll_deg: number;
  pitch_deg: number;
  yaw_relative_deg: number;
}

export interface PodEnvironment extends ValidValue {
  temperature_valid: boolean;
  temperature_c: number;
  humidity_valid: boolean;
  humidity_percent: number;
}

export interface PodPosition {
  local_valid: boolean;
  local_x_m: number;
  local_y_m: number;
  global_valid: boolean;
  latitude: number;
  longitude: number;
  altitude_valid: boolean;
  altitude_m: number;
}

export interface PodCapabilities {
  barometer: boolean;
  environment: boolean;
  imu: boolean;
  geophone: boolean;
  local_position: boolean;
  global_position: boolean;
}

export interface PodGeophone extends ValidValue {
  instant_uv: number;
  rms_uv: number;
  peak_uv: number;
  noise_rms_uv: number;
  snr_db: number;
}

export interface PodHeightReference {
  state: 'not_established' | 'valid' | 'rejected_moving' | 'sensor_invalid';
  generation: number;
  relative_z_mm: number;
}

export interface MainHeightReference {
  state: 'not_established' | 'valid' | 'rejected_unstable' | 'sensor_invalid';
  generation: number;
  relative_z_mm: number;
}

export interface DeviceMainStatus {
  height_reference?: MainHeightReference;
  pressure_pa?: number;
  temperature_mc?: number;
  sensor_present?: { bmp280: boolean };
  sensor_valid?: { bmp280: boolean };
}

export type NodeLinkStatus = 'NOT_CONNECTED' | 'CONNECTED' | 'READY' | 'MEASURING' | 'STALE';

export interface DeviceNodeLink {
  channel: number;
  node: string;
  status: NodeLinkStatus | (string & {});
}

export interface DeviceSnapshotFreshness {
  valid: boolean;
  age_ms: number;
  stale: boolean;
}

export interface DevicePod {
  id: number;
  state: string;
  node_link_status?: NodeLinkStatus;
  capabilities: PodCapabilities;
  barometer: PodBarometer;
  environment: PodEnvironment;
  imu: PodImu;
  geophone: PodGeophone;
  position: PodPosition;
  height_reference?: PodHeightReference;
  sensor_health?: {
    current: boolean;
    imu_valid: boolean;
    barometer_valid: boolean;
    imu_fault: boolean;
    barometer_fault: boolean;
    imu_calibrated: boolean;
  };
}

export interface DeviceSystemStatus {
  state: 'NOT_READY' | 'DEGRADED' | 'READY' | 'FAULT' | string;
  mode: string;
  uptime_ms: number;
  acquisition_ready: boolean;
  ready_pods: number;
  required_pods: number;
}

export type HardwareModuleId =
  | 'sd_card'
  | 'ad7606'
  | 'sensor_mux'
  | 'main_xiao'
  | 'barometer'
  | 'power_monitor';

export type HardwareModuleState = 'unknown' | 'present' | 'absent' | 'fault' | 'degraded';

export type HardwareReasonCode =
  | 'ready'
  | 'no_response'
  | 'no_filesystem'
  | 'mount_failed'
  | 'read_probe_failed'
  | 'topology_probe_ok'
  | 'topology_probe_fault'
  | 'no_upstream_response'
  | 'sensor_invalid'
  | 'first_sample_ok'
  | 'no_valid_sample'
  | 'busy_responds'
  | 'no_busy_response'
  | 'presence_probe_failed'
  | 'blocked_5v_rail_low'
  | 'init_failed'
  | 'start_failed'
  | 'sensor_ok'
  | 'upstream_target_assigned'
  | 'upstream_topology_fault'
  | 'none'
  | 'unrecognized';

export interface HardwareModuleStatus {
  /** Wire values stay open so older/newer firmware payloads remain parseable. */
  id: string;
  state: string;
  required: boolean;
  reason: string;
  reason_code?: string;
}

export interface DeviceHardwareStatus {
  modules: HardwareModuleStatus[];
  required_missing: boolean;
}

export type NrfLogNodeId = 'main' | `remote${0 | 1 | 2 | 3 | 4 | 5 | 6 | 7}`;

export interface NrfLogCounters {
  overwritten_total: number;
  lost_internal_total: number;
  wire_gap_total: number;
  local_drop_total: number;
}

export interface NrfLogRecord {
  token: string;
  boot_id: number;
  seq: number;
  uptime_ms: number;
  esp_rx_uptime_ms: number;
  event_id: number;
  severity: number;
  subsystem: number;
  arg0: number;
  arg1: number;
}

export interface NrfLogPage {
  ok: true;
  node: NrfLogNodeId;
  channel: number | null;
  records: NrfLogRecord[];
  next_token: string | null;
  reset: boolean;
  counters: NrfLogCounters;
  logs_unsupported: boolean;
  boot_id: number | null;
}

export interface NrfLogNodeSummary {
  node: NrfLogNodeId;
  channel: number | null;
  present: boolean;
  logs_unsupported: boolean;
  boot_id: number | null;
  stored: number;
  warn_count: number;
  error_count: number;
  fatal_count: number;
  wire_gap_total: number;
  local_drop_total: number;
  overwritten_total: number;
}

export type NrfLogBadRequestReason = 'bad_node' | 'bad_limit' | 'bad_token';

export interface SeismicCadenceEvidence {
  available: boolean;
  timer_ticks: number;
  conversion_attempts: number;
  collapsed_periods: number;
}

export interface DeviceAcquisitionStatus {
  running: boolean;
  requested_running: boolean;
  hardware_ready: boolean;
  source: string;
  last_error?: number;
  last_error_name?: string;
  sample_rate_hz: number;
  samples_recorded: number;
  recording_ms: number;
  cadence?: SeismicCadenceEvidence & {
    last_successful_sample_interval_us: number;
    max_successful_sample_interval_us: number;
  };
  serial?: {
    suspicious: boolean;
    pattern: string;
    quality?: string;
    stream_state?: string;
    stream_stuck?: boolean;
    /** MAX-242/245 AD7606 +5 V rail interlock: 'ok' | 'unverified' | 'rail_low' | 'overcurrent' | 'unknown'. */
    power_gate?: string;
    power_fault_latched?: boolean;
    power_fault_rail_v?: number;
    /** MAX-245: rail current when an over-current fault latched (A). */
    power_fault_current_a?: number;
    suspicious_streak: number;
    repeated_frame_streak: number;
    all_zero_frames: number;
    all_ff_frames: number;
    all_identical_frames: number;
    varied_frames: number;
    digital_interference_frames?: number;
    quality_window_frames?: number;
    quality_bad_ratio?: number;
    quality_digital_interference_ratio?: number;
    busy_level: number;
  };
  signal?: {
    quality: string;
    channel_count?: number;
    rail_hit_frames: number;
    clip_frames: number;
    quality_window_frames: number;
    rail_hit_ratio: number;
    clip_ratio: number;
  };
}

/** One INA3221 channel (MAX-245 adds the software over-current state). */
export interface DevicePowerChannel {
  id: number;
  name: string;
  valid: boolean;
  voltage_v: number;
  current_a: number;
  power_w: number;
  overcurrent?: 'none' | 'warning' | 'critical' | string;
  overcurrent_latched?: boolean;
  age_ms?: number;
  stale?: boolean;
}

export interface DeviceStatus {
  /** Missing on older firmware; absence is treated as storage being available. */
  storage_available?: boolean;
  storage_busy?: boolean;
  system: DeviceSystemStatus;
  /** Absent on firmware that predates MAX-383 hardware inventory reporting. */
  hardware?: DeviceHardwareStatus;
  power: ValidValue & {
    voltage_v: number;
    current_a: number;
    power_w: number;
    channels?: DevicePowerChannel[];
    age_ms?: number;
    stale?: boolean;
    /** Stable /api/status enum strings for the user-facing power state. */
    state?: DevicePowerState;
    battery_present?: boolean;
    battery_percent_valid?: boolean;
    battery_percent_estimate?: number | null;
    battery_voltage_v?: number;
    battery_current_a?: number;
    usb_present?: boolean;
    charging?: boolean;
    battery_full?: boolean;
    /** MAX-183: normal | warning | grace | shutdown. */
    low_battery?: string;
    shutdown_in_s?: number;
    /** MAX-181: VOLTAGE_ONLY | COULOMB_TRACKED | FULL_ANCHORED | INVALID. */
    battery_soc_source?: string;
  };
  environment: ValidValue & { temperature_c: number; humidity_percent: number; pressure_pa: number };
  mux: ValidValue & { active_channel: number; active_pod: number };
  main?: DeviceMainStatus;
  pods: DevicePod[];
  node_links?: DeviceNodeLink[];
  topology?: DeviceSnapshotFreshness;
  acquisition: DeviceAcquisitionStatus;
  recording?: RecordingState;
  recording_gate?: RecordingGateStatus;
  measurement?: MeasurementServiceStatus;
  object_store?: ObjectStoreState;
  recording_sessions?: RecordingSessionCollection;
  workflow?: DeviceWorkflowStatus;
  ranging?: DeviceRangingStatus;
  /** Absent on firmware that predates MAX-308 log summaries. */
  nrf_logs?: NrfLogNodeSummary[];
}

export type DevicePowerState =
  | 'USB_ONLY_NO_BATTERY'
  | 'USB_CHARGING'
  | 'USB_BATTERY_READY'
  | 'BATTERY'
  | 'BATTERY_LOW'
  | 'BATTERY_CRITICAL'
  | 'UNKNOWN';

export interface SeismicSnapshot {
  source: string;
  sample_rate_hz: number;
  count: number;
  frame_count: number;
  requested_span_ms: number;
  span_ms: number;
  decimation_factor: number;
  buffered_frames: number;
  total_frames: number;
  points: SavedSeismicEnvelopePoint[];
}

export interface SavedSeismicEnvelopeChannel {
  id: number;
  min_uv: number;
  max_uv: number;
}

export interface SavedSeismicEnvelopePoint {
  timestamp_us: number;
  end_timestamp_us: number;
  channels: SavedSeismicEnvelopeChannel[];
}

export interface SavedSeismicSnapshot {
  object_id: string;
  session_id: string;
  source: string;
  status: RecordingSessionStatus;
  complete: boolean;
  readable: boolean;
  recovered: boolean;
  sample_rate_hz: number;
  channel_count: number;
  samples_written: number;
  dropped_frames: number;
  dropped_frames_available: boolean;
  recording_ms: number;
  recording_ms_available: boolean;
  cadence: SeismicCadenceEvidence;
  source_frame_count: number;
  start_frame: number;
  frame_count: number;
  count: number;
  start_timestamp_us: number;
  end_timestamp_us: number;
  points: SavedSeismicEnvelopePoint[];
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function isNonNegativeInteger(value: unknown): value is number {
  return isFiniteNumber(value) && Number.isInteger(value) && value >= 0;
}

function normalizeCadenceEvidence(value: unknown): SeismicCadenceEvidence {
  if (value === undefined) {
    return { available: false, timer_ticks: 0, conversion_attempts: 0, collapsed_periods: 0 };
  }
  if (typeof value !== 'object' || value === null) {
    throw new Error('ESP seismic cadence evidence is invalid.');
  }

  const cadence = value as Record<string, unknown>;
  if (typeof cadence.available !== 'boolean') {
    throw new Error('ESP seismic cadence availability is invalid.');
  }
  if (!cadence.available) {
    return { available: false, timer_ticks: 0, conversion_attempts: 0, collapsed_periods: 0 };
  }

  const timerTicks = cadence.timer_ticks;
  const attempts = cadence.conversion_attempts;
  const collapsed = cadence.collapsed_periods;
  if (
    !isNonNegativeInteger(timerTicks) ||
    !isNonNegativeInteger(attempts) ||
    !isNonNegativeInteger(collapsed) ||
    attempts > timerTicks ||
    collapsed !== timerTicks - attempts
  ) {
    throw new Error('ESP seismic cadence counters are inconsistent.');
  }

  return {
    available: true,
    timer_ticks: timerTicks,
    conversion_attempts: attempts,
    collapsed_periods: collapsed,
  };
}

function normalizeSavedSessionStatus(payload: Record<string, unknown>): {
  status: RecordingSessionStatus;
  complete: boolean;
  readable: boolean;
  recovered: boolean;
} {
  if (typeof payload.complete !== 'boolean') {
    throw new Error('ESP saved seismic completion state is invalid.');
  }

  const rawStatus = payload.status;
  const status: RecordingSessionStatus = rawStatus === undefined
    ? (payload.complete ? 'complete' : 'unknown')
    : rawStatus === 'complete' || rawStatus === 'error' || rawStatus === 'recovered_error'
      ? rawStatus
      : 'unknown';

  if (rawStatus !== undefined && status === 'unknown') {
    throw new Error('ESP saved seismic terminal status is invalid.');
  }

  const expectedComplete = status === 'complete';
  if (status !== 'unknown' && payload.complete !== expectedComplete) {
    throw new Error('ESP saved seismic completion/status state is inconsistent.');
  }

  const readable = payload.readable === undefined
    ? payload.complete === true
    : payload.readable;
  if (typeof readable !== 'boolean') {
    throw new Error('ESP saved seismic readability state is invalid.');
  }

  const expectedRecovered = status === 'recovered_error';
  const recovered = payload.recovered === undefined
    ? expectedRecovered
    : payload.recovered;
  if (typeof recovered !== 'boolean' || (status !== 'unknown' && recovered !== expectedRecovered)) {
    throw new Error('ESP saved seismic recovery state is invalid.');
  }

  if (!readable || status === 'unknown') {
    throw new Error('ESP saved seismic session is not terminal/readable.');
  }

  return {
    status,
    complete: payload.complete,
    readable,
    recovered,
  };
}

function normalizeSeismicEnvelopePoints(value: unknown, count: number): SavedSeismicEnvelopePoint[] {
  if (!Array.isArray(value) || value.length !== count) {
    throw new Error('ESP seismic API returned an invalid envelope point count.');
  }

  let previousStart = -Infinity;
  let previousEnd = -Infinity;
  return value.map((rawPoint) => {
    if (typeof rawPoint !== 'object' || rawPoint === null) {
      throw new Error('ESP seismic API returned an invalid envelope point.');
    }
    const point = rawPoint as Record<string, unknown>;
    const timestamp = point.timestamp_us;
    const endTimestamp = point.end_timestamp_us;
    const rawChannels = point.channels;
    if (
      !isFiniteNumber(timestamp) || !isFiniteNumber(endTimestamp) ||
      endTimestamp < timestamp || timestamp < previousStart || endTimestamp < previousEnd ||
      !Array.isArray(rawChannels) || rawChannels.length !== 4
    ) {
      throw new Error('ESP seismic API returned invalid envelope timing or channels.');
    }
    previousStart = timestamp;
    previousEnd = endTimestamp;
    const channels = rawChannels.map((rawChannel, index) => {
      if (typeof rawChannel !== 'object' || rawChannel === null) {
        throw new Error('ESP seismic API returned an invalid envelope channel.');
      }
      const channel = rawChannel as Record<string, unknown>;
      if (
        channel.id !== index + 1 || !isFiniteNumber(channel.min_uv) ||
        !isFiniteNumber(channel.max_uv) || channel.min_uv > channel.max_uv
      ) {
        throw new Error('ESP seismic API returned an invalid envelope range.');
      }
      return { id: index + 1, min_uv: channel.min_uv, max_uv: channel.max_uv };
    });
    return { timestamp_us: timestamp, end_timestamp_us: endTimestamp, channels };
  });
}

/** Validate the four-channel time-window envelope returned for live seismic. */
export function normalizeSeismicSnapshot(value: unknown): SeismicSnapshot {
  if (typeof value !== 'object' || value === null) {
    throw new Error('ESP seismic API returned a non-object payload.');
  }

  const payload = value as Record<string, unknown>;
  const source = payload.source;
  const sampleRate = payload.sample_rate_hz;
  const count = payload.count;
  const frameCount = payload.frame_count;
  const requestedSpanMs = payload.requested_span_ms;
  const spanMs = payload.span_ms;
  const decimationFactor = payload.decimation_factor;
  const bufferedFrames = payload.buffered_frames;
  const totalFrames = payload.total_frames;
  const rawPoints = payload.points;

  if (
    typeof source !== 'string' ||
    !isFiniteNumber(sampleRate) || sampleRate < 0 ||
    !isNonNegativeInteger(count) ||
    !isNonNegativeInteger(bufferedFrames) ||
    !isNonNegativeInteger(frameCount) || frameCount > bufferedFrames ||
    !isNonNegativeInteger(requestedSpanMs) || requestedSpanMs < 1 ||
    !isNonNegativeInteger(spanMs) ||
    !isNonNegativeInteger(decimationFactor) ||
    (count === 0 ? decimationFactor !== 0 : decimationFactor < 1) ||
    !isNonNegativeInteger(totalFrames) ||
    count > 1200 || count > frameCount ||
    !Array.isArray(rawPoints) || rawPoints.length !== count
  ) {
    throw new Error('ESP seismic API returned an invalid snapshot shape.');
  }
  const points = normalizeSeismicEnvelopePoints(rawPoints, count);

  return {
    source,
    sample_rate_hz: sampleRate,
    count,
    frame_count: frameCount,
    requested_span_ms: requestedSpanMs,
    span_ms: spanMs,
    decimation_factor: decimationFactor,
    buffered_frames: bufferedFrames,
    total_frames: totalFrames,
    points,
  };
}

/** Validate the bounded min/max envelope returned for one terminal readable session. */
export function normalizeSavedSeismicSnapshot(value: unknown): SavedSeismicSnapshot {
  if (typeof value !== 'object' || value === null) {
    throw new Error('ESP saved seismic API returned a non-object payload.');
  }

  const payload = value as Record<string, unknown>;
  const requiredStrings = [payload.object_id, payload.session_id, payload.source];
  const requiredCounts = [
    payload.sample_rate_hz,
    payload.channel_count,
    payload.samples_written,
    payload.dropped_frames,
    payload.recording_ms,
    payload.source_frame_count,
    payload.start_frame,
    payload.frame_count,
    payload.count,
    payload.start_timestamp_us,
    payload.end_timestamp_us,
  ];

  if (
    !requiredStrings.every((candidate) => typeof candidate === 'string' && candidate.length > 0) ||
    !requiredCounts.every(isNonNegativeInteger) ||
    (payload.sample_rate_hz as number) === 0 ||
    payload.channel_count !== 4 ||
    (payload.start_frame as number) + (payload.frame_count as number) > (payload.source_frame_count as number) ||
    !Array.isArray(payload.points) ||
    payload.points.length !== (payload.count as number) ||
    (payload.count as number) > 1200
  ) {
    throw new Error('ESP saved seismic API returned an invalid session shape.');
  }

  const terminal = normalizeSavedSessionStatus(payload);
  const droppedFramesAvailable = payload.dropped_frames_available === undefined
    ? true
    : payload.dropped_frames_available;
  const recordingMsAvailable = payload.recording_ms_available === undefined
    ? true
    : payload.recording_ms_available;
  if (typeof droppedFramesAvailable !== 'boolean' || typeof recordingMsAvailable !== 'boolean') {
    throw new Error('ESP saved seismic metric availability is invalid.');
  }

  const cadence = normalizeCadenceEvidence(payload.cadence);
  let previousStart = -Infinity;
  let previousEnd = -Infinity;
  const points = payload.points.map((point) => {
    if (typeof point !== 'object' || point === null) {
      throw new Error('ESP saved seismic API returned an invalid envelope point.');
    }

    const candidate = point as Record<string, unknown>;
    if (!isFiniteNumber(candidate.timestamp_us) || !Number.isInteger(candidate.timestamp_us)) {
      throw new Error('ESP saved seismic API returned an invalid envelope timestamp.');
    }

    const endTimestamp = candidate.end_timestamp_us === undefined
      ? candidate.timestamp_us
      : candidate.end_timestamp_us;
    if (
      !isFiniteNumber(endTimestamp) ||
      !Number.isInteger(endTimestamp) ||
      endTimestamp < candidate.timestamp_us ||
      candidate.timestamp_us < previousStart ||
      endTimestamp < previousEnd
    ) {
      throw new Error('ESP saved seismic API returned invalid envelope timing geometry.');
    }
    previousStart = candidate.timestamp_us;
    previousEnd = endTimestamp;

    if (!Array.isArray(candidate.channels) || candidate.channels.length !== 4) {
      throw new Error('ESP saved seismic API returned an invalid envelope channel count.');
    }

    const channels = candidate.channels.map((channel, index) => {
      if (typeof channel !== 'object' || channel === null) {
        throw new Error('ESP saved seismic API returned an invalid envelope channel.');
      }

      const channelValue = channel as Record<string, unknown>;
      if (
        channelValue.id !== index + 1 ||
        !isFiniteNumber(channelValue.min_uv) ||
        !isFiniteNumber(channelValue.max_uv) ||
        channelValue.min_uv > channelValue.max_uv
      ) {
        throw new Error('ESP saved seismic API returned an invalid envelope range.');
      }

      return {
        id: index + 1,
        min_uv: channelValue.min_uv,
        max_uv: channelValue.max_uv,
      };
    });

    return {
      timestamp_us: candidate.timestamp_us,
      end_timestamp_us: endTimestamp,
      channels,
    };
  });

  return {
    object_id: payload.object_id as string,
    session_id: payload.session_id as string,
    source: payload.source as string,
    status: terminal.status,
    complete: terminal.complete,
    readable: terminal.readable,
    recovered: terminal.recovered,
    sample_rate_hz: payload.sample_rate_hz as number,
    channel_count: 4,
    samples_written: payload.samples_written as number,
    dropped_frames: payload.dropped_frames as number,
    dropped_frames_available: droppedFramesAvailable,
    recording_ms: payload.recording_ms as number,
    recording_ms_available: recordingMsAvailable,
    cadence,
    source_frame_count: payload.source_frame_count as number,
    start_frame: payload.start_frame as number,
    frame_count: payload.frame_count as number,
    count: payload.count as number,
    start_timestamp_us: payload.start_timestamp_us as number,
    end_timestamp_us: payload.end_timestamp_us as number,
    points,
  };
}

export interface PhoneLocationInput {
  latitude: number;
  longitude: number;
  accuracy_m: number;
  altitude_m: number | null;
  altitude_accuracy_m: number | null;
  captured_at_ms: number;
}

export function activeObject(store: ObjectStoreState | null): ObjectProfile | null {
  if (store === null) return null;
  return store.objects.find((object) => object.active || object.id === store.active_id) ?? null;
}

export function connectedPodCount(status: DeviceStatus | null): number {
  if (status === null) return 0;
  return status.pods.filter((pod) => pod.state !== 'DISCONNECTED').length;
}

export function recordingWorkflowStepAllowed(status: DeviceStatus | null): boolean {
  const step = status?.workflow?.step;
  return step === 'RECORD_READY' || step === 'RECORDING';
}

/* The firmware prints a non-finite float as JSON null, so a coordinate can arrive as null. */
export function formatCoordinate(value: number | null): string {
  return typeof value === 'number' && Number.isFinite(value) ? value.toFixed(6) : '—';
}

export function normalizeCableLength(value: number): number {
  if (!Number.isFinite(value)) return 5;
  return Math.min(100, Math.max(0.1, Math.round(value * 100) / 100));
}
