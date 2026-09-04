import type { DeviceStatus, PhoneLocationInput, SeismicSnapshot } from '../domain/device';

export const DEVICE_API_FAST_TIMEOUT_MS = 3000;
export const DEVICE_API_EXTENDED_TIMEOUT_MS = 8000;
export const DEVICE_API_CONTROL_TIMEOUT_MS = 8000;

export interface ControlResult {
  ok: boolean;
  action: string;
  object_id?: string;
  error?: string;
}

const statusRequests = new Map<string, Promise<DeviceStatus>>();
let extendedStatusRequest: Promise<DeviceStatus> | null = null;

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

async function jsonRequest<T>(url: string, init: RequestInit = {}, timeoutMs = DEVICE_API_FAST_TIMEOUT_MS): Promise<T> {
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

export function fetchSeismic(points = 180): Promise<SeismicSnapshot> {
  const bounded = Math.max(16, Math.min(1200, Math.round(points)));
  return jsonRequest<SeismicSnapshot>(`/api/seismic?points=${bounded}`, {}, DEVICE_API_FAST_TIMEOUT_MS);
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
