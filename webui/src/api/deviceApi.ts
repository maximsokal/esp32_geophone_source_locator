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
  error?: string;
}

const statusRequests = new Map<string, Promise<DeviceStatus>>();
let extendedStatusRequest: Promise<DeviceStatus> | null = null;
let seismicRequest: Promise<SeismicSnapshot> | null = null;
let apiRequestActive = false;
let pendingApiRequests = 0;
const apiRequestQueue: Array<() => void> = [];

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function responseError(body: unknown, response: Response): string {
  if (typeof body === 'object' && body !== null && 'error' in body) {
    return String((body as { error: unknown }).error);
  }
  if (typeof body === 'string' && body.trim().length > 0) return body.trim().slice(0, 240);
  return response.statusText || 'request failed';
}

async function performJsonRequest<T>(
  url: string,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
): Promise<T> {
  const method = (init.method ?? 'GET').toUpperCase();
  const controller = new AbortController();
  const timeout = globalThis.setTimeout(() => controller.abort(), timeoutMs);

  try {
    let response: Response;
    let text: string;
    try {
      response = await fetch(url, {
        cache: 'no-store',
        credentials: 'same-origin',
        ...init,
        signal: controller.signal,
        headers: {
          Accept: 'application/json',
          ...(init.body === undefined ? {} : { 'Content-Type': 'application/json' }),
          ...(init.headers ?? {}),
        },
      });
      text = await response.text();
    }
    catch (reason) {
      if (controller.signal.aborted) {
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
      throw new Error(`ESP API ${response.status} ${method} ${url}: ${responseError(body, response)}`);
    }

    return body as T;
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

function queuedApiRequest<T>(operation: () => Promise<T>): Promise<T> {
  /*
   * ESP-IDF's HTTPS server executes application handlers serially. Opening a
   * second fetch while the first request is doing an SD write only starts a
   * second TLS/socket timeout; it cannot make the ESP finish sooner. Keep JSON,
   * seismic and binary scan transfers behind the same browser-side gate.
   */
  pendingApiRequests += 1;

  return new Promise<T>((resolve, reject) => {
    const start = (): void => {
      apiRequestActive = true;
      void operation().then(
        (value) => {
          finish();
          resolve(value);
        },
        (reason) => {
          finish();
          reject(reason);
        },
      );
    };

    const finish = (): void => {
      pendingApiRequests = Math.max(0, pendingApiRequests - 1);
      const next = apiRequestQueue.shift();
      if (next === undefined) {
        apiRequestActive = false;
        return;
      }
      next();
    };

    if (apiRequestActive) apiRequestQueue.push(start);
    else start();
  });
}

function jsonRequest<T>(
  url: string,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
): Promise<T> {
  return queuedApiRequest(() => performJsonRequest<T>(url, init, timeoutMs));
}

function scanControlUrl(action: 'save' | 'load' | 'delete', objectId: string, sessionId: string): string {
  const query = new URLSearchParams({ scan: action, object_id: objectId, session_id: sessionId });
  return `/api/control?${query.toString()}`;
}

async function performScanSave(objectId: string, sessionId: string, artifact: Uint8Array): Promise<ControlResult> {
  const url = scanControlUrl('save', objectId, sessionId);
  const controller = new AbortController();
  const timeout = globalThis.setTimeout(() => controller.abort(), DEVICE_API_SCAN_TIMEOUT_MS);
  try {
    let response: Response;
    let text: string;
    try {
      const body = artifact.slice().buffer;
      response = await fetch(url, {
        method: 'POST',
        cache: 'no-store',
        credentials: 'same-origin',
        signal: controller.signal,
        headers: {
          Accept: 'application/json',
          'Content-Type': 'application/octet-stream',
        },
        body,
      });
      text = await response.text();
    }
    catch (reason) {
      if (controller.signal.aborted) throw new Error(`ESP scan save timeout (${DEVICE_API_SCAN_TIMEOUT_MS} ms)`);
      throw new Error(`ESP scan save network error: ${errorText(reason)}`);
    }

    let body: unknown = null;
    if (text.length > 0) {
      try { body = JSON.parse(text); }
      catch { body = text; }
    }
    if (!response.ok) throw new Error(`ESP scan save ${response.status}: ${responseError(body, response)}`);
    return body as ControlResult;
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

async function performScanLoad(objectId: string, sessionId: string): Promise<Uint8Array | null> {
  const url = scanControlUrl('load', objectId, sessionId);
  const controller = new AbortController();
  const timeout = globalThis.setTimeout(() => controller.abort(), DEVICE_API_SCAN_TIMEOUT_MS);
  try {
    let response: Response;
    try {
      response = await fetch(url, {
        method: 'POST',
        cache: 'no-store',
        credentials: 'same-origin',
        signal: controller.signal,
        headers: { Accept: 'application/octet-stream' },
      });
    }
    catch (reason) {
      if (controller.signal.aborted) throw new Error(`ESP scan load timeout (${DEVICE_API_SCAN_TIMEOUT_MS} ms)`);
      throw new Error(`ESP scan load network error: ${errorText(reason)}`);
    }

    if (response.status === 404) return null;
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`ESP scan load ${response.status}: ${responseError(text, response)}`);
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
  }, DEVICE_API_CONTROL_TIMEOUT_MS);
}

export function saveObjectScanArtifact(objectId: string, sessionId: string, artifact: Uint8Array): Promise<ControlResult> {
  return queuedApiRequest(() => performScanSave(objectId, sessionId, artifact));
}

export function loadObjectScanArtifact(objectId: string, sessionId: string): Promise<Uint8Array | null> {
  return queuedApiRequest(() => performScanLoad(objectId, sessionId));
}

export function deleteObjectScanArtifact(objectId: string, sessionId: string): Promise<ControlResult> {
  return jsonRequest<ControlResult>(scanControlUrl('delete', objectId, sessionId), {
    method: 'POST',
  }, DEVICE_API_SCAN_TIMEOUT_MS);
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
  ).then(normalizeSavedSeismicSnapshot);
}

export interface LocalizationRunPosition {
  x_m: number;
  y_m: number;
  z_m: number;
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
  x_mm?: number;
  y_mm?: number;
  z_mm?: number;
  computed_at_ms?: number;
}

export interface LocalizationSessionGeometry {
  available: boolean;
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
}

export interface LocalizationSessionCsPairs {
  available: boolean;
  pairs: LocalizationCsPair[];
}

export interface LocalizationPickChannel {
  channel: number;
  participating: boolean;
  status: 'accepted' | 'rejected_no_trigger' | 'rejected_no_refinement' | 'rejected_low_snr';
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
