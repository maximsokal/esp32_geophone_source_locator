import type { DeviceStatus, PhoneLocationInput, SeismicSnapshot } from '../domain/device';

export const DEVICE_API_FAST_TIMEOUT_MS = 5000;
export const DEVICE_API_EXTENDED_TIMEOUT_MS = 12000;
export const DEVICE_API_CONTROL_TIMEOUT_MS = 12000;

export interface ControlResult {
  ok: boolean;
  action: string;
  object_id?: string;
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

function jsonRequest<T>(
  url: string,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
): Promise<T> {
  /*
   * ESP-IDF's HTTPS server executes application handlers serially. Opening a
   * second fetch while the first request is doing a multi-second FATFS write
   * only starts another TLS/socket timeout on the phone; it cannot make the
   * ESP finish sooner. Serialize the control-plane requests in the browser so
   * timeout accounting starts when a request can actually run.
   */
  pendingApiRequests += 1;

  return new Promise<T>((resolve, reject) => {
    const start = (): void => {
      apiRequestActive = true;
      void performJsonRequest<T>(url, init, timeoutMs).then(
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

export function fetchSeismic(points = 180): Promise<SeismicSnapshot> {
  if (seismicRequest !== null) return seismicRequest;

  const bounded = Math.max(16, Math.min(1200, Math.round(points)));
  let request: Promise<SeismicSnapshot>;
  request = jsonRequest<SeismicSnapshot>(
    `/api/seismic?points=${bounded}`,
    {},
    DEVICE_API_FAST_TIMEOUT_MS,
  ).then(
    (value) => {
      if (seismicRequest === request) seismicRequest = null;
      return value;
    },
    (reason) => {
      if (seismicRequest === request) seismicRequest = null;
      throw reason;
    },
  );
  seismicRequest = request;
  return request;
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
