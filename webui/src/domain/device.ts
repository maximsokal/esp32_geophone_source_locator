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
  barometer: PodBarometer;
  imu: PodImu;
  geophone: PodGeophone;
}

export interface DeviceStatus {
  system: { state: string; mode: string; uptime_ms: number };
  power: ValidValue & { voltage_v: number; current_a: number; power_w: number };
  environment: ValidValue & { temperature_c: number; humidity_percent: number; pressure_pa: number };
  mux: ValidValue & { active_channel: number; active_pod: number };
  pods: DevicePod[];
  acquisition: { running: boolean; sample_rate_hz: number; samples_recorded: number; recording_ms: number };
  object_store?: ObjectStoreState;
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
