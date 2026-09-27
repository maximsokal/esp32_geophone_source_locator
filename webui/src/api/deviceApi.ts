import type { LocalizationJobStatus } from '../domain/localizationJob';
import {
  normalizeSeismicSnapshot,
  normalizeSavedSeismicSnapshot,
  type DeviceStatus,
  type PhoneLocationInput,
  type SavedSeismicSnapshot,
  type SeismicSnapshot,
} from '../domain/device';

export const DEVICE_API_FAST_TIMEOUT_MS = 5000;
export const DEVICE_API_EXTENDED_TIMEOUT_MS = 12000;
export const DEVICE_API_CONTROL_TIMEOUT_MS = 12000;
export const DEVICE_API_SCAN_TIMEOUT_MS = 30000;

export interface ControlResult {
  ok: boolean;
  action: string;
  object_id?: string;
  bytes?: number;
  error_class?: string;
  error?: string;
  outcome?: 'blocked_recording';
  busy_reason?: 'recording_active' | string;
}

export class ScanStorageBlockedRecordingError extends Error {
  readonly outcome = 'blocked_recording' as const;
  readonly busy_reason = 'recording_active' as const;

  constructor() {
    super('Recording in progress — scan storage is paused');
    this.name = 'ScanStorageBlockedRecordingError';
  }
}

export class BulkTransferPreemptedError extends Error {
  constructor() {
    super('Bulk transfer paused for a critical control action. Retry when ready.');
    this.name = 'BulkTransferPreemptedError';
  }
}

function recordingBlocked(body: unknown, status: number): boolean {
  return status === 409 && typeof body === 'object' && body !== null &&
    'busy_reason' in body && (body as { busy_reason?: unknown }).busy_reason === 'recording_active';
}

function parsedResponseBody(text: string): unknown {
  if (text.length === 0) return null;
  try { return JSON.parse(text) as unknown; }
  catch { return text; }
}

function blockedControlResult(body: unknown): ControlResult {
  const action = typeof body === 'object' && body !== null && 'action' in body
    ? String((body as { action: unknown }).action)
    : 'scan_save';
  return { ok: false, action, outcome: 'blocked_recording', busy_reason: 'recording_active' };
}

const statusRequests = new Map<string, Promise<DeviceStatus>>();
let extendedStatusRequest: Promise<DeviceStatus> | null = null;
let seismicRequest: Promise<SeismicSnapshot> | null = null;
let pendingApiRequests = 0;

type ApiRequestLane = 'critical' | 'normal' | 'bulk';

interface ApiRequestContext {
  controller: AbortController;
  preempted: boolean;
  timedOut: boolean;
}

interface QueuedApiRequest {
  lane: ApiRequestLane;
  operation: (context: ApiRequestContext) => Promise<unknown>;
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  context?: ApiRequestContext;
}

const apiRequestQueues: Record<ApiRequestLane, QueuedApiRequest[]> = {
  critical: [],
  normal: [],
  bulk: [],
};
let activeApiRequest: QueuedApiRequest | null = null;

/* Keep safety and calibration controls out of the bulk transfer lane. */
const CRITICAL_CONTROL_ACTIONS = new Set([
  'measurement_start',
  'measurement_stop',
  'acquisition_start',
  'acquisition_stop',
  'calibrate_heights',
]);

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function responseError(body: unknown, response: Response): string {
  if (typeof body === 'object' && body !== null && 'error' in body) {
    if ('error_class' in body && (body as { error_class?: unknown }).error_class === 'acquisition_unavailable') {
      return `Acquisition unavailable: ${String((body as { error: unknown }).error)}`;
    }
    return String((body as { error: unknown }).error);
  }
  if (typeof body === 'string' && body.trim().length > 0) return body.trim().slice(0, 240);
  return response.statusText || 'request failed';
}

async function performJsonRequest<T>(
  url: string,
  context: ApiRequestContext,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
): Promise<T> {
  const method = (init.method ?? 'GET').toUpperCase();
  const timeout = globalThis.setTimeout(() => {
    context.timedOut = true;
    context.controller.abort();
  }, timeoutMs);

  try {
    let response: Response;
    let text: string;
    try {
      response = await fetch(url, {
        cache: 'no-store',
        credentials: 'same-origin',
        ...init,
        signal: context.controller.signal,
        headers: {
          Accept: 'application/json',
          ...(init.body === undefined ? {} : { 'Content-Type': 'application/json' }),
          ...(init.headers ?? {}),
        },
      });
      text = await response.text();
    }
    catch (reason) {
      if (context.timedOut) {
        throw new Error(`ESP API timeout (${method} ${url}, ${timeoutMs} ms)`);
      }
      throw new Error(`ESP API network error (${method} ${url}): ${errorText(reason)}`);
    }

    let body: unknown = null;
    if (text.length > 0) {
      try {
        body = JSON.parse(text);
      }
      catch {
        body = text;
      }
    }

    if (!response.ok) {
      if (recordingBlocked(body, response.status)) throw new ScanStorageBlockedRecordingError();
      throw new Error(`ESP API ${response.status} ${method} ${url}: ${responseError(body, response)}`);
    }

    return body as T;
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

function nextApiRequest(): QueuedApiRequest | undefined {
  return apiRequestQueues.critical.shift()
    ?? apiRequestQueues.normal.shift()
    ?? apiRequestQueues.bulk.shift();
}

function startNextApiRequest(): void {
  if (activeApiRequest !== null) return;
  const request = nextApiRequest();
  if (request === undefined) return;

  const context: ApiRequestContext = {
    controller: new AbortController(),
    preempted: false,
    timedOut: false,
  };
  request.context = context;
  activeApiRequest = request;

  const finish = (fulfilled: boolean, value: unknown): void => {
    pendingApiRequests = Math.max(0, pendingApiRequests - 1);
    activeApiRequest = null;
    if (context.preempted) {
      request.reject(new BulkTransferPreemptedError());
    } else if (fulfilled) {
      request.resolve(value);
    } else {
      request.reject(value);
    }
    startNextApiRequest();
  };

  try {
    void request.operation(context).then(
      (value) => finish(true, value),
      (reason) => finish(false, reason),
    );
  } catch (reason) {
    finish(false, reason);
  }
}

function queuedApiRequest<T>(
  operation: (context: ApiRequestContext) => Promise<T>,
  lane: ApiRequestLane = 'normal',
): Promise<T> {
  /*
   * ESP-IDF's HTTPS server executes application handlers serially. Opening a
   * second fetch while the first request is doing an SD write only starts a
   * second TLS/socket timeout; it cannot make the ESP finish sooner. Keep JSON,
   * seismic and binary scan transfers behind the same browser-side gate.
   */
  pendingApiRequests += 1;

  return new Promise<T>((resolve, reject) => {
    const request: QueuedApiRequest = {
      lane,
      operation,
      resolve: (value) => resolve(value as T),
      reject,
    };
    apiRequestQueues[lane].push(request);

    const active = activeApiRequest;
    const context = active?.context;
    if (lane === 'critical' && active?.lane === 'bulk' && context !== undefined &&
        !context.timedOut && !context.controller.signal.aborted) {
      context.preempted = true;
      context.controller.abort();
    }
    startNextApiRequest();
  });
}

function jsonRequest<T>(
  url: string,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
  lane: ApiRequestLane = 'normal',
): Promise<T> {
  return queuedApiRequest((context) => performJsonRequest<T>(url, context, init, timeoutMs), lane);
}

export type ScanStoreKind = 'scan' | 'occupancy' | 'preview' | 'registration';

function scanControlUrl(action: 'save' | 'load' | 'delete' | 'info', objectId: string, sessionId: string, kind: ScanStoreKind = 'scan'): string {
  const query = new URLSearchParams({ scan: action, object_id: objectId, session_id: sessionId });
  if (kind !== 'scan') query.set('kind', kind);
  return `/api/control?${query.toString()}`;
}

async function performScanSave(
  objectId: string,
  sessionId: string,
  artifact: Uint8Array,
  context: ApiRequestContext,
  kind: ScanStoreKind = 'scan',
): Promise<ControlResult> {
  const url = scanControlUrl('save', objectId, sessionId, kind);
  const timeout = globalThis.setTimeout(() => {
    context.timedOut = true;
    context.controller.abort();
  }, DEVICE_API_SCAN_TIMEOUT_MS);
  try {
    let response: Response;
    let text: string;
    try {
      const body = artifact.slice().buffer;
      response = await fetch(url, {
        method: 'POST',
        cache: 'no-store',
        credentials: 'same-origin',
        signal: context.controller.signal,
        headers: {
          Accept: 'application/json',
          'Content-Type': kind === 'registration' ? 'application/json; charset=utf-8' : 'application/octet-stream',
        },
        body,
      });
      text = await response.text();
    }
    catch (reason) {
      if (context.timedOut) throw new Error(`ESP scan save timeout (${DEVICE_API_SCAN_TIMEOUT_MS} ms)`);
      throw new Error(`ESP scan save network error: ${errorText(reason)}`);
    }

    const body = parsedResponseBody(text);
    if (!response.ok) {
      if (recordingBlocked(body, response.status)) return blockedControlResult(body);
      throw new Error(`ESP scan save ${response.status}: ${responseError(body, response)}`);
    }
    return body as ControlResult;
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

async function performScanLoad(
  objectId: string,
  sessionId: string,
  context: ApiRequestContext,
  kind: ScanStoreKind = 'scan',
): Promise<Uint8Array | null> {
  const url = scanControlUrl('load', objectId, sessionId, kind);
  const timeout = globalThis.setTimeout(() => {
    context.timedOut = true;
    context.controller.abort();
  }, DEVICE_API_SCAN_TIMEOUT_MS);
  try {
    let response: Response;
    try {
      response = await fetch(url, {
        method: 'POST',
        cache: 'no-store',
        credentials: 'same-origin',
        signal: context.controller.signal,
        headers: { Accept: 'application/octet-stream' },
      });
    }
    catch (reason) {
      if (context.timedOut) throw new Error(`ESP scan load timeout (${DEVICE_API_SCAN_TIMEOUT_MS} ms)`);
      throw new Error(`ESP scan load network error: ${errorText(reason)}`);
    }

    if (response.status === 404) return null;
    if (!response.ok) {
      const text = await response.text();
      const body = parsedResponseBody(text);
      if (recordingBlocked(body, response.status)) throw new ScanStorageBlockedRecordingError();
      throw new Error(`ESP scan load ${response.status}: ${responseError(body, response)}`);
    }
    return new Uint8Array(await response.arrayBuffer());
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

export function deviceApiHasPendingRequest(): boolean {
  return pendingApiRequests > 0;
}

export function fetchDeviceStatus(
  includeObjects = false,
  sessionsObjectId?: string | null,
): Promise<DeviceStatus> {
  /* An extended response is a strict superset of the fast RAM status. */
  if (!includeObjects && !sessionsObjectId && extendedStatusRequest !== null) {
    return extendedStatusRequest;
  }

  const query = new URLSearchParams();
  if (includeObjects) query.set('objects', '1');
  if (sessionsObjectId) query.set('sessions', sessionsObjectId);
  const suffix = query.size > 0 ? `?${query.toString()}` : '';
  const url = `/api/status${suffix}`;

  const existing = statusRequests.get(url);
  if (existing !== undefined) return existing;

  const extended = includeObjects || Boolean(sessionsObjectId);
  const timeoutMs = extended ? DEVICE_API_EXTENDED_TIMEOUT_MS : DEVICE_API_FAST_TIMEOUT_MS;

  let request: Promise<DeviceStatus>;
  request = jsonRequest<DeviceStatus>(url, {}, timeoutMs).then(
    (value) => {
      statusRequests.delete(url);
      if (extendedStatusRequest === request) extendedStatusRequest = null;
      return value;
    },
    (reason) => {
      statusRequests.delete(url);
      if (extendedStatusRequest === request) extendedStatusRequest = null;
      throw reason;
    },
  );
  statusRequests.set(url, request);
  if (extended) extendedStatusRequest = request;
  return request;
}

export function sendControl(
  action: string,
  payload: Record<string, unknown> = {},
): Promise<ControlResult> {
  return jsonRequest<ControlResult>('/api/control', {
    method: 'POST',
    body: JSON.stringify({ action, ...payload }),
  }, DEVICE_API_CONTROL_TIMEOUT_MS, CRITICAL_CONTROL_ACTIONS.has(action) ? 'critical' : 'normal');
}

export function saveObjectScanArtifact(objectId: string, sessionId: string, artifact: Uint8Array): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, artifact, context), 'bulk');
}

/**
 * MAX-162: the compact FREE/OCCUPIED occupancy artifact derived from the
 * session's saved scan, stored beside it (deleted together with the scan).
 */
export function saveSessionOccupancyArtifact(objectId: string, sessionId: string, artifact: Uint8Array): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, artifact, context, 'occupancy'), 'bulk');
}

/** MAX-24/MAX-187: bounded surface-voxel preview for the ESP32 TFT. */
export function saveSessionPreviewArtifact(objectId: string, sessionId: string, artifact: Uint8Array): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, artifact, context, 'preview'), 'bulk');
}

export function saveSessionRegistration(objectId: string, sessionId: string, json: string): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, new TextEncoder().encode(json), context, 'registration'), 'bulk');
}

export function loadSessionRegistration(objectId: string, sessionId: string): Promise<string | null> {
  return queuedApiRequest(async (context) => {
    const bytes = await performScanLoad(objectId, sessionId, context, 'registration');
    return bytes === null ? null : new TextDecoder().decode(bytes);
  }, 'bulk');
}

export function deleteSessionRegistration(objectId: string, sessionId: string): Promise<ControlResult> {
  return queuedApiRequest((context) => performJsonRequest<ControlResult>(
    scanControlUrl('delete', objectId, sessionId, 'registration'),
    context,
    { method: 'POST' },
    DEVICE_API_EXTENDED_TIMEOUT_MS,
  ), 'bulk');
}

export function loadObjectScanArtifact(objectId: string, sessionId: string): Promise<Uint8Array | null> {
  return queuedApiRequest((context) => performScanLoad(objectId, sessionId, context), 'bulk');
}

export function loadSessionPreviewArtifact(objectId: string, sessionId: string): Promise<Uint8Array | null> {
  return queuedApiRequest((context) => performScanLoad(objectId, sessionId, context, 'preview'), 'bulk');
}

export interface ScanArtifactInfo {
  present: boolean;
  bytes: number;
}

/** MAX-169: presence/size of a session's scan (or occupancy) without downloading it. */
export async function fetchScanInfo(objectId: string, sessionId: string, kind: ScanStoreKind = 'scan'): Promise<ScanArtifactInfo> {
  try {
    const result = await jsonRequest<{ ok: boolean; bytes?: number; error?: string }>(
      scanControlUrl('info', objectId, sessionId, kind),
      { method: 'POST' },
      DEVICE_API_EXTENDED_TIMEOUT_MS,
    );
    if (result.ok) return { present: true, bytes: result.bytes ?? 0 };
    if (result.error === 'ESP_ERR_NOT_FOUND') return { present: false, bytes: 0 };
    throw new Error(result.error ?? 'scan info failed');
  }
  catch (reason) {
    const text = reason instanceof Error ? reason.message : String(reason);
    if (text.includes('ESP API 404') || text.includes('ESP_ERR_NOT_FOUND')) return { present: false, bytes: 0 };
    throw reason;
  }
}

export function deleteObjectScanArtifact(objectId: string, sessionId: string): Promise<ControlResult> {
  return jsonRequest<ControlResult>(scanControlUrl('delete', objectId, sessionId), {
    method: 'POST',
  }, DEVICE_API_SCAN_TIMEOUT_MS, 'bulk');
}

export function fetchSeismic(points = 180): Promise<SeismicSnapshot> {
  if (seismicRequest !== null) return seismicRequest;

  const bounded = Math.max(16, Math.min(1200, Math.round(points)));
  let request: Promise<SeismicSnapshot>;
  request = jsonRequest<SeismicSnapshot>(
    `/api/seismic?points=${bounded}`,
    {},
    DEVICE_API_FAST_TIMEOUT_MS,
  ).then((value) => normalizeSeismicSnapshot(value)).finally(() => {
      if (seismicRequest === request) seismicRequest = null;
  });
  seismicRequest = request;
  return request;
}

export interface SavedSeismicRequestOptions {
  startFrame?: number;
  frameCount?: number;
  points?: number;
}

export function fetchSavedSeismicSession(
  objectId: string,
  sessionId: string,
  options: SavedSeismicRequestOptions = {},
): Promise<SavedSeismicSnapshot> {
  if (objectId.trim().length === 0 || sessionId.trim().length === 0) {
    return Promise.reject(new Error('Saved seismic object and session are required.'));
  }

  const query = new URLSearchParams({
    object_id: objectId,
    session_id: sessionId,
  });
  if (options.startFrame !== undefined) {
    query.set('start_frame', String(Math.max(0, Math.floor(options.startFrame))));
  }
  if (options.frameCount !== undefined) {
    query.set('frame_count', String(Math.max(0, Math.floor(options.frameCount))));
  }
  if (options.points !== undefined) {
    const bounded = Math.max(16, Math.min(1200, Math.round(options.points)));
    query.set('points', String(bounded));
  }

  return jsonRequest<unknown>(
    `/api/seismic/session?${query.toString()}`,
    {},
    DEVICE_API_EXTENDED_TIMEOUT_MS,
    'bulk',
  ).then(normalizeSavedSeismicSnapshot);
}

export interface LocalizationRunPosition {
  x_m: number;
  y_m: number;
  z_m: number;
}

/** Posterior region of a Bayesian run (sensor frame, metres). */
export interface LocalizationRunUncertainty {
  /** Per-axis posterior standard deviations. */
  std_m: LocalizationRunPosition;
  /** Probability held by the credible region, e.g. 0.9. */
  credible_level: number;
  /** Region size: volume for a 3D search, area for a planar one (the other is 0). */
  credible_volume_m3: number;
  credible_area_m2: number;
  credible_min: LocalizationRunPosition;
  credible_max: LocalizationRunPosition;
  /** >= 1: every sigma was scaled by this because the fit's chi-square showed them too small. */
  sigma_inflation?: number;
}

export interface LocalizationRun {
  schema_version: number;
  object_id: string;
  session_id: string;
  computed_at_ms: number;
  kind: 'individual_method' | 'joint_fusion';
  method_name: string;
  method_version: number;
  channel_mask_used: number;
  channel_mask_rejected: number;
  dimensionality: 'insufficient' | 'planar_2d' | 'capable_3d';
  geometry_computed_at_ms: number;
  propagation_model_name: string;
  propagation_speed_mps: number;
  propagation_speed_uncertainty_mps: number;
  confidence_score: number;
  /** MAX-212: redundancy of a pick-based fit; 0 = exactly determined (unverified), -1/absent = n/a. */
  degrees_of_freedom?: number;
  /** Methods that estimate the event's own velocities (TDCAS, PSAFUVS, SUEA, PSP; eikonal and Bayesian when the speed is uncertain): P (or single-phase) and S; absent = not estimated. */
  estimated_p_velocity_mps?: number;
  estimated_s_velocity_mps?: number;
  /** Bayesian methods: the posterior's spread and credible region; absent = not computed. */
  uncertainty?: LocalizationRunUncertainty;
  solved: boolean;
  source_position?: LocalizationRunPosition;
  emission_time_us?: number;
  rms_residual_us?: number;
  iterations?: number;
  converged?: boolean;
  rejection_reason?: string;
}

export interface LocalizationSessionPodGeometry {
  pod: number;
  computed: boolean;
  leveled: boolean;
  assigned?: boolean;
  node_id?: number | null;
  hardware_id?: string | null;
  position_source?: 'none' | 'channel_sounding' | 'channel_sounding_baro';
  x_mm?: number;
  y_mm?: number;
  z_mm?: number;
  /** Largest |solved - measured| CS distance for this pod (MAX-205). */
  residual_mm?: number;
  /** MAX-144: 1-sigma position uncertainty (mm); 0 = n/a, 2147483647 = undetermined. */
  uncertainty_mm?: number;
  computed_at_ms?: number;
  source_sample_uptime_ms?: number;
  esp_receipt_uptime_ms?: number;
  source_age_ms_at_start?: number | null;
  height_reference_generation?: number;
}

export interface LocalizationGeometryProvenance {
  solver?: string;
  solver_version?: string;
  refined?: boolean;
  refine_iterations?: number;
  topology_generation?: number;
  cs_membership_generation?: number;
  main_height_reference_generation?: number;
  setup_id?: string;
  start_uptime_ms?: number;
}

export interface LocalizationSessionGeometry {
  available: boolean;
  z_policy_effective?: 'planar' | 'barometric_relative';
  z_fallback_reason?: string;
  provenance?: LocalizationGeometryProvenance;
  pods: LocalizationSessionPodGeometry[];
}

export interface LocalizationCsPair {
  node_a: number;
  node_b: number;
  valid: boolean;
  distance_mm: number;
  confidence: number;
  attempts: number;
  valid_samples: number;
  /** MAX-206: robust spread of the accepted samples (mm) and outliers dropped. */
  spread_mm?: number;
  rejected_samples?: number;
  state?: string;
  last_failure?: string;
  provider_quality_flags?: number;
  scheduler_quality_flags?: number;
}

export interface LocalizationSessionCsPairs {
  available: boolean;
  pairs: LocalizationCsPair[];
}

export interface LocalizationPickChannel {
  channel: number;
  participating: boolean;
  status: 'accepted' | 'rejected_no_trigger' | 'rejected_no_refinement' | 'rejected_low_snr' | 'rejected_inconsistent';
  method: string;
  arrival_sample_index: number;
  arrival_time_s: number;
  snr: number;
  uncertainty_samples: number;
}

export interface LocalizationPickRecord {
  schema_version: number;
  object_id: string;
  session_id: string;
  computed_at_ms: number;
  channels: LocalizationPickChannel[];
}

export interface LocalizationRunListing {
  object_id: string;
  session_id: string;
  geometry: LocalizationSessionGeometry;
  cs_pairs: LocalizationSessionCsPairs;
  picks: LocalizationPickRecord[];
  runs: LocalizationRun[];
}

export function fetchLocalizationRuns(objectId: string, sessionId: string): Promise<LocalizationRunListing> {
  const query = new URLSearchParams({ object_id: objectId, session_id: sessionId });
  return jsonRequest<LocalizationRunListing>(
    `/api/localization/runs?${query.toString()}`,
    {},
    DEVICE_API_EXTENDED_TIMEOUT_MS,
  );
}

/** The background "Calculate source" job: progress, measured timings and stack. */
export function fetchLocalizationJob(): Promise<LocalizationJobStatus> {
  return jsonRequest<LocalizationJobStatus>('/api/localization/runs?job=status', {}, DEVICE_API_CONTROL_TIMEOUT_MS);
}

export function capturePhoneLocation(): Promise<PhoneLocationInput> {
  if (!('geolocation' in navigator)) {
    return Promise.reject(new Error('Geolocation is not supported by this browser.'));
  }

  return new Promise((resolve, reject) => {
    navigator.geolocation.getCurrentPosition(
      (position) => resolve({
        latitude: position.coords.latitude,
        longitude: position.coords.longitude,
        accuracy_m: position.coords.accuracy,
        altitude_m: position.coords.altitude,
        altitude_accuracy_m: position.coords.altitudeAccuracy,
        captured_at_ms: position.timestamp,
      }),
      (error) => reject(new Error(`Phone GPS unavailable: ${error.message}`)),
      {
        enableHighAccuracy: true,
        maximumAge: 0,
        timeout: 15000,
      },
    );
  });
}
