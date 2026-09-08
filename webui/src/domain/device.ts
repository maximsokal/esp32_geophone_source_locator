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

export interface RecordingSessionSummary {
  session_id: string;
  complete: boolean;
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
}

export interface DeviceStatus {
  system: DeviceSystemStatus;
  power: ValidValue & { voltage_v: number; current_a: number; power_w: number };
  environment: ValidValue & { temperature_c: number; humidity_percent: number; pressure_pa: number };
  mux: ValidValue & { active_channel: number; active_pod: number };
  pods: DevicePod[];
  acquisition: DeviceAcquisitionStatus;
  recording?: RecordingState;
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
