import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  capturePhoneLocation,
  fetchDeviceStatus,
  loadObjectScanArtifact,
  saveObjectScanArtifact,
  sendControl,
} from '../../src/api/deviceApi';

type VirtualEsp32State = {
  wifiConnected: boolean;
  reconnects: number;
  location: Record<string, unknown> | null;
  scan: Uint8Array | null;
  requests: Array<{ method: string; path: string; contentType: string | undefined; bodyLength: number }>;
};

type VirtualEsp32 = VirtualEsp32State & {
  baseUrl: string;
  disconnectWifi: () => void;
  reconnectWifi: () => void;
  close: () => Promise<void>;
};

async function readBody(request: any): Promise<Uint8Array> {
  const chunks: Uint8Array[] = [];
  for await (const chunk of request) chunks.push(new Uint8Array(chunk));
  const length = chunks.reduce((total, chunk) => total + chunk.byteLength, 0);
  const body = new Uint8Array(length);
  let offset = 0;
  for (const chunk of chunks) {
    body.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return body;
}

function sendJson(response: any, status: number, value: unknown): void {
  const body = JSON.stringify(value);
  response.writeHead(status, {
    'Content-Type': 'application/json',
    'Content-Length': Buffer.byteLength(body),
  });
  response.end(body);
}

async function startVirtualEsp32(): Promise<VirtualEsp32> {
  const { createServer } = await import('node:http');
  const state: VirtualEsp32State = {
    wifiConnected: true,
    reconnects: 0,
    location: null,
    scan: null,
    requests: [],
  };

  const server = createServer((request: any, response: any) => {
    void (async () => {
      const url = new URL(
        request.url ?? '/',
        `http://${request.headers.host ?? '127.0.0.1'}`,
      );
      const body = request.method === 'POST' ? await readBody(request) : new Uint8Array();
      state.requests.push({
        method: request.method ?? '',
        path: `${url.pathname}${url.search}`,
        contentType: request.headers['content-type'],
        bodyLength: body.byteLength,
      });

      if (!state.wifiConnected) {
        sendJson(response, 503, { ok: false, error: 'wifi disconnected' });
        return;
      }

      if (request.method === 'GET' && url.pathname === '/api/status') {
        sendJson(response, 200, {
          system: {
            state: 'READY',
            mode: 'WIFI',
            uptime_ms: 1234,
            acquisition_ready: true,
            ready_pods: 0,
            required_pods: 0,
          },
          power: { valid: false, voltage_v: 0, current_a: 0, power_w: 0 },
          environment: { valid: false, temperature_c: 0, humidity_percent: 0, pressure_pa: 0 },
          mux: { valid: false, active_channel: -1, active_pod: -1 },
          pods: [],
          acquisition: {
            running: false,
            requested_running: false,
            hardware_ready: true,
            source: 'VIRTUAL_AD7606',
            last_error: 0,
            last_error_name: 'ESP_OK',
            sample_rate_hz: 500,
            samples_recorded: 0,
            recording_ms: 0,
          },
          object_store: {
            available: true,
            active_id: 'OBJ_0001',
            default_cable_lengths_m: [5, 5, 5, 5],
            objects: [{
              id: 'OBJ_0001',
              name: 'VIRTUAL_HIL',
              active: true,
              location: state.location ?? {
                valid: false,
                latitude: 0,
                longitude: 0,
                accuracy_m: 0,
                altitude_valid: false,
                altitude_m: 0,
                altitude_accuracy_m: 0,
                captured_at_ms: 0,
                source: '',
              },
              cable_lengths_m: [5, 5, 5, 5],
            }],
          },
        });
        return;
      }

      if (request.method === 'POST' && url.pathname === '/api/control') {
        const scanAction = url.searchParams.get('scan');
        if (scanAction === 'save') {
          state.scan = body;
          sendJson(response, 200, {
            ok: true,
            action: 'scan_save',
            object_id: url.searchParams.get('object_id'),
            bytes: body.byteLength,
          });
          return;
        }
        if (scanAction === 'load') {
          if (state.scan === null) {
            sendJson(response, 404, { ok: false, error: 'scan not found' });
            return;
          }
          response.writeHead(200, {
            'Content-Type': 'application/octet-stream',
            'Content-Length': state.scan.byteLength,
          });
          for (let offset = 0; offset < state.scan.byteLength; offset += 3) {
            response.write(state.scan.slice(offset, offset + 3));
          }
          response.end();
          return;
        }

        const payload = JSON.parse(new TextDecoder().decode(body)) as Record<string, any>;
        if (payload.action !== 'set_object_location') {
          sendJson(response, 400, { ok: false, error: 'unsupported virtual action' });
          return;
        }
        state.location = {
          valid: true,
          latitude: payload.latitude,
          longitude: payload.longitude,
          accuracy_m: payload.accuracy_m,
          altitude_valid: payload.altitude_m !== null && payload.altitude_m !== undefined,
          altitude_m: payload.altitude_m ?? 0,
          altitude_accuracy_m: payload.altitude_accuracy_m ?? 0,
          captured_at_ms: payload.captured_at_ms,
          source: 'PHONE_GPS',
        };
        sendJson(response, 200, { ok: true, action: payload.action, object_id: payload.object_id });
        return;
      }

      sendJson(response, 404, { ok: false, error: 'route not found' });
    })().catch((error: unknown) => {
      sendJson(response, 500, { ok: false, error: String(error) });
    });
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (address === null || typeof address === 'string') {
    await new Promise<void>((resolve) => server.close(() => resolve()));
    throw new Error('virtual ESP32 server did not expose a TCP port');
  }

  return {
    get wifiConnected() { return state.wifiConnected; },
    get reconnects() { return state.reconnects; },
    get location() { return state.location; },
    get scan() { return state.scan; },
    get requests() { return state.requests; },
    baseUrl: `http://127.0.0.1:${address.port}`,
    disconnectWifi: () => { state.wifiConnected = false; },
    reconnectWifi: () => {
      state.wifiConnected = true;
      state.reconnects += 1;
    },
    close: () => new Promise<void>((resolve, reject) => {
      server.close((error?: Error) => error === undefined ? resolve() : reject(error));
    }),
  };
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('virtual ESP32-S3 HTTP/Wi-Fi boundary', () => {
  it('round-trips phone GPS, survives reconnect, and streams an emulated SD artifact', async () => {
    const esp = await startVirtualEsp32();
    const nativeFetch = globalThis.fetch.bind(globalThis);
    vi.stubGlobal('fetch', (input: RequestInfo | URL, init?: RequestInit) => {
      const raw = typeof input === 'string'
        ? input
        : input instanceof URL
          ? input.toString()
          : input.url;
      return nativeFetch(new URL(raw, esp.baseUrl), init);
    });
    vi.stubGlobal('navigator', {
      geolocation: {
        getCurrentPosition: (success: PositionCallback) => success({
          coords: {
            latitude: 50.4501,
            longitude: 30.5234,
            accuracy: 3.2,
            altitude: 179.4,
            altitudeAccuracy: 5.5,
            heading: null,
            speed: null,
          },
          timestamp: 123456789,
        } as GeolocationPosition),
      },
    });

    try {
      const phonePosition = await capturePhoneLocation();
      await expect(sendControl('set_object_location', {
        object_id: 'OBJ_0001',
        ...phonePosition,
      })).resolves.toMatchObject({ ok: true, action: 'set_object_location' });

      const status = await fetchDeviceStatus(true);
      expect(status.object_store?.objects[0].location).toMatchObject({
        valid: true,
        latitude: 50.4501,
        longitude: 30.5234,
        source: 'PHONE_GPS',
      });

      esp.disconnectWifi();
      await expect(fetchDeviceStatus(true)).rejects.toThrow('ESP API 503 GET /api/status');
      esp.reconnectWifi();

      const restored = await fetchDeviceStatus(true);
      expect(restored.object_store?.objects[0].location).toMatchObject({
        latitude: 50.4501,
        longitude: 30.5234,
      });
      expect(esp.reconnects).toBe(1);

      const artifact = new Uint8Array([0, 1, 2, 255, 8, 13, 21]);
      await expect(saveObjectScanArtifact('OBJ_0001', 'S000001', artifact))
        .resolves.toMatchObject({ ok: true, action: 'scan_save', bytes: artifact.byteLength });
      await expect(loadObjectScanArtifact('OBJ_0001', 'S000001'))
        .resolves.toEqual(artifact);

      expect(esp.requests.some((request) => request.method === 'GET' && request.path.startsWith('/api/status?objects=1'))).toBe(true);
      expect(esp.requests.some((request) => request.path.startsWith('/api/control?scan=save'))).toBe(true);
      expect(esp.requests.some((request) => request.path.startsWith('/api/control?scan=load'))).toBe(true);
    }
    finally {
      await esp.close();
    }
  });
});
