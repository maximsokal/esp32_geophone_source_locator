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

export interface ObjectProfile {
  id: string;
  name: string;
  active: boolean;
  location: ObjectLocation;
  cable_lengths_m: number[];
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

export interface RecordingSessionSummary {
  session_id: string;
  complete: boolean;
  source?: string;
  sample_rate_hz: number;
  channel_count: number;
  samples_written: number;
  dropped_frames: number;
  recording_ms: number;
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

export interface DevicePod {
  id: number;
  state: string;
  capabilities: PodCapabilities;
  barometer: PodBarometer;
  environment: PodEnvironment;
  imu: PodImu;
  geophone: PodGeophone;
  position: PodPosition;
}

export interface DeviceSystemStatus {
  state: 'NOT_READY' | 'DEGRADED' | 'READY' | 'FAULT' | string;
  mode: string;
  uptime_ms: number;
  acquisition_ready: boolean;
  ready_pods: number;
  required_pods: number;
}

export interface DeviceAcquisitionStatus {
  running: boolean;
  requested_running: boolean;
  hardware_ready: boolean;
  source: string;
  last_error: number;
  last_error_name: string;
  sample_rate_hz: number;
  samples_recorded: number;
  recording_ms: number;
  serial?: {
    suspicious: boolean;
    pattern: string;
    quality?: string;
    stream_state?: string;
    stream_stuck?: boolean;
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

export interface DeviceStatus {
  storage_busy?: boolean;
  system: DeviceSystemStatus;
  power: ValidValue & { voltage_v: number; current_a: number; power_w: number };
  environment: ValidValue & { temperature_c: number; humidity_percent: number; pressure_pa: number };
  mux: ValidValue & { active_channel: number; active_pod: number };
  pods: DevicePod[];
  acquisition: DeviceAcquisitionStatus;
  recording?: RecordingState;
  measurement?: MeasurementServiceStatus;
  object_store?: ObjectStoreState;
  recording_sessions?: RecordingSessionCollection;
}

export interface SeismicChannel {
  id: number;
  samples_uv: number[];
}

export interface SeismicSnapshot {
  source: string;
  sample_rate_hz: number;
  count: number;
  buffered_frames: number;
  total_frames: number;
  timestamps_ms: number[];
  channels: SeismicChannel[];
}

export interface SavedSeismicEnvelopeChannel {
  id: number;
  min_uv: number;
  max_uv: number;
}

export interface SavedSeismicEnvelopePoint {
  timestamp_us: number;
  channels: SavedSeismicEnvelopeChannel[];
}

export interface SavedSeismicSnapshot {
  object_id: string;
  session_id: string;
  source: string;
  complete: boolean;
  sample_rate_hz: number;
  channel_count: number;
  samples_written: number;
  dropped_frames: number;
  recording_ms: number;
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

/** Validate the four-channel /api/seismic boundary before rendering it. */
export function normalizeSeismicSnapshot(value: unknown): SeismicSnapshot {
  if (typeof value !== 'object' || value === null) {
    throw new Error('ESP seismic API returned a non-object payload.');
  }

  const payload = value as Record<string, unknown>;
  const source = payload.source;
  const sampleRate = payload.sample_rate_hz;
  const count = payload.count;
  const bufferedFrames = payload.buffered_frames;
  const totalFrames = payload.total_frames;
  const timestamps = payload.timestamps_ms;
  const channels = payload.channels;

  if (
    typeof source !== 'string' ||
    !isFiniteNumber(sampleRate) || sampleRate < 0 ||
    !isNonNegativeInteger(count) ||
    !isNonNegativeInteger(bufferedFrames) ||
    !isNonNegativeInteger(totalFrames) ||
    !Array.isArray(timestamps) || timestamps.length !== count ||
    !timestamps.every(isFiniteNumber) ||
    !Array.isArray(channels) || channels.length !== 4
  ) {
    throw new Error('ESP seismic API returned an invalid snapshot shape.');
  }

  const normalizedChannels = channels.map((channel, index) => {
    if (typeof channel !== 'object' || channel === null) {
      throw new Error('ESP seismic API returned an invalid channel.');
    }

    const candidate = channel as Record<string, unknown>;
    if (
      candidate.id !== index + 1 ||
      !Array.isArray(candidate.samples_uv) ||
      candidate.samples_uv.length !== count ||
      !candidate.samples_uv.every(isFiniteNumber)
    ) {
      throw new Error('ESP seismic API returned an invalid channel shape.');
    }

    return {
      id: index + 1,
      samples_uv: candidate.samples_uv,
    };
  });

  return {
    source,
    sample_rate_hz: sampleRate,
    count,
    buffered_frames: bufferedFrames,
    total_frames: totalFrames,
    timestamps_ms: timestamps,
    channels: normalizedChannels,
  };
}

/** Validate the bounded min/max envelope returned for one completed session. */
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
    payload.complete !== true ||
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

  const points = payload.points.map((point) => {
    if (typeof point !== 'object' || point === null) {
      throw new Error('ESP saved seismic API returned an invalid envelope point.');
    }

    const candidate = point as Record<string, unknown>;
    if (!isFiniteNumber(candidate.timestamp_us) || !Number.isInteger(candidate.timestamp_us)) {
      throw new Error('ESP saved seismic API returned an invalid envelope timestamp.');
    }

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
      channels,
    };
  });

  return {
    object_id: payload.object_id as string,
    session_id: payload.session_id as string,
    source: payload.source as string,
    complete: true,
    sample_rate_hz: payload.sample_rate_hz as number,
    channel_count: 4,
    samples_written: payload.samples_written as number,
    dropped_frames: payload.dropped_frames as number,
    recording_ms: payload.recording_ms as number,
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

export function formatCoordinate(value: number): string {
  return value.toFixed(6);
}

export function normalizeCableLength(value: number): number {
  if (!Number.isFinite(value)) return 5;
  return Math.min(100, Math.max(0.1, Math.round(value * 100) / 100));
}
