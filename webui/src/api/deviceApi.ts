import type { DeviceStatus, PhoneLocationInput, SeismicSnapshot } from '../domain/device';

async function jsonRequest<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, {
    cache: 'no-store',
    credentials: 'same-origin',
    ...init,
    headers: {
      Accept: 'application/json',
      ...(init?.body === undefined ? {} : { 'Content-Type': 'application/json' }),
      ...(init?.headers ?? {}),
    },
  });

  const text = await response.text();
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
    const message = typeof body === 'object' && body !== null && 'error' in body
      ? String((body as { error: unknown }).error)
      : `${response.status} ${response.statusText}`;
    throw new Error(message);
  }

  return body as T;
}

export function fetchDeviceStatus(
  includeObjects = false,
  sessionsObjectId?: string | null,
): Promise<DeviceStatus> {
  const query = new URLSearchParams();
  if (includeObjects) query.set('objects', '1');
  if (sessionsObjectId) query.set('sessions', sessionsObjectId);
  const suffix = query.size > 0 ? `?${query.toString()}` : '';
  return jsonRequest<DeviceStatus>(`/api/status${suffix}`);
}

export async function sendControl(action: string, payload: Record<string, unknown> = {}): Promise<void> {
  await jsonRequest('/api/control', {
    method: 'POST',
    body: JSON.stringify({ action, ...payload }),
  });
}

export function fetchSeismic(points = 180): Promise<SeismicSnapshot> {
  const bounded = Math.max(16, Math.min(1200, Math.round(points)));
  return jsonRequest<SeismicSnapshot>(`/api/seismic?points=${bounded}`);
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
