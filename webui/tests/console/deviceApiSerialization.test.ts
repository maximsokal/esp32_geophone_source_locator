import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  BulkTransferPreemptedError,
  capturePhoneLocation,
  fetchDeviceStatus,
  fetchSavedSeismicSession,
  fetchSeismic,
  saveObjectScanArtifact,
  sendControl,
} from '../../src/api/deviceApi';
import { envelopePolylinePoints } from '../../src/features/seismic/SavedSeismicViewer';

function jsonResponse(body: unknown, status = 200, statusText = 'OK'): Response {
  return new Response(JSON.stringify(body), {
    status,
    statusText,
    headers: { 'Content-Type': 'application/json' },
  });
}

function validDeviceStatus() {
  return {
    system: { state: 'NOT_READY', mode: 'NORMAL', uptime_ms: 0, acquisition_ready: false, ready_pods: 0, required_pods: 0 },
    power: { valid: false, voltage_v: 0, current_a: 0, power_w: 0 },
    environment: { valid: false, temperature_c: 0, humidity_percent: 0, pressure_pa: 0 },
    mux: { valid: false, active_channel: -1, active_pod: -1 },
    pods: [],
    acquisition: { running: false, requested_running: false, hardware_ready: false, source: 'NONE', sample_rate_hz: 0, samples_recorded: 0, recording_ms: 0 },
  };
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('serialized ESP API traffic', () => {
  it('captures all phone GPS fields before the control POST', async () => {
    const getCurrentPosition = vi.fn((success: PositionCallback) => {
      success({
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
      } as GeolocationPosition);
    });
    vi.stubGlobal('navigator', { geolocation: { getCurrentPosition } });

    await expect(capturePhoneLocation()).resolves.toEqual({
      latitude: 50.4501,
      longitude: 30.5234,
      accuracy_m: 3.2,
      altitude_m: 179.4,
      altitude_accuracy_m: 5.5,
      captured_at_ms: 123456789,
    });
    expect(getCurrentPosition).toHaveBeenCalledWith(
      expect.any(Function),
      expect.any(Function),
      { enableHighAccuracy: true, maximumAge: 0, timeout: 15000 },
    );
  });

  it('preempts bulk for a critical control and starts it before a queued status poll', async () => {
    let bulkSignal!: AbortSignal;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url);
      if (url.includes('scan=save')) {
        bulkSignal = init?.signal as AbortSignal;
        return new Promise<Response>((_resolve, reject) => {
          bulkSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        });
      }
      if (url === '/api/control') return Promise.resolve(jsonResponse({ ok: true, action: 'measurement_stop' }));
      return Promise.resolve(jsonResponse(validDeviceStatus()));
    });
    vi.stubGlobal('fetch', fetchMock);

    const bulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1, 2, 3]));
    const status = fetchDeviceStatus(false);
    const control = sendControl('measurement_stop');

    expect(bulkSignal.aborted).toBe(true);
    await expect(bulk).rejects.toBeInstanceOf(BulkTransferPreemptedError);
    await control;
    await status;

    expect(starts).toEqual([
      '/api/control?scan=save&object_id=OBJ_0001&session_id=S000001',
      '/api/control',
      '/api/status',
    ]);
  });

  it('coalesces live seismic requests while one sample fetch is pending', async () => {
    let resolveSeismic!: (response: Response) => void;
    const fetchMock = vi.fn(() => new Promise<Response>((resolve) => { resolveSeismic = resolve; }));
    vi.stubGlobal('fetch', fetchMock);

    const first = fetchSeismic({ spanMs: 440, maxPoints: 220 });
    const second = fetchSeismic({ spanMs: 440, maxPoints: 220 });
    await Promise.resolve();

    expect(first).toBe(second);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveSeismic(jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 0,
      frame_count: 0,
      requested_span_ms: 440,
      span_ms: 0,
      decimation_factor: 0,
      buffered_frames: 0,
      total_frames: 0,
      points: [],
    }));
    await Promise.all([first, second]);
    expect(fetchMock).toHaveBeenCalledWith('/api/seismic?span_ms=440&max_points=220', expect.anything());
  });

  it('keeps the default live request at the former 500 Hz sample span', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 0,
      frame_count: 0,
      requested_span_ms: 440,
      span_ms: 0,
      decimation_factor: 0,
      buffered_frames: 0,
      total_frames: 0,
      points: [],
    }));
    vi.stubGlobal('fetch', fetchMock);

    await fetchSeismic();
    expect(fetchMock).toHaveBeenCalledWith('/api/seismic?span_ms=440&max_points=220', expect.anything());
  });

  it('carries a signed API impulse through polling normalization into waveform points', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 2,
      frame_count: 3,
      requested_span_ms: 440,
      span_ms: 6,
      decimation_factor: 2,
      buffered_frames: 3,
      total_frames: 103,
      points: [
        { timestamp_us: 1000, end_timestamp_us: 1002, channels: [1, 2, 3, 4].map((id) => ({
          id, min_uv: id === 1 ? -100 : 0, max_uv: id === 1 ? 100 : 0,
        })) },
        { timestamp_us: 1004, end_timestamp_us: 1004, channels: [1, 2, 3, 4].map((id) => ({
          id, min_uv: id === 1 ? -50 : 0, max_uv: id === 1 ? -50 : 0,
        })) },
      ],
    }));
    vi.stubGlobal('fetch', fetchMock);

    const snapshot = await fetchSeismic({ spanMs: 440, maxPoints: 220 });
    const geometry = envelopePolylinePoints(
      snapshot.points,
      0,
      720,
      120,
      1000,
      1004,
    );

    expect(snapshot.points[0].channels[0]).toEqual({ id: 1, min_uv: -100, max_uv: 100 });
    expect(geometry.min).toContain('M 0.0');
    expect(geometry.max).toContain('M 0.0');
    expect(geometry.min).toContain('L 360.0');
    expect(geometry.max).toContain('L 360.0');
  });

  it('rejects a malformed seismic channel before it reaches the renderer', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 1,
      frame_count: 1,
      requested_span_ms: 440,
      span_ms: 2,
      decimation_factor: 1,
      buffered_frames: 1,
      total_frames: 1,
      points: [{ timestamp_us: 1000, end_timestamp_us: 1000, channels: [1, 2, 3, 8].map((id) => ({
        id, min_uv: 1, max_uv: 1,
      })) }],
    }));
    vi.stubGlobal('fetch', fetchMock);

    await expect(fetchSeismic({ spanMs: 440, maxPoints: 220 })).rejects.toThrow('invalid envelope range');

    fetchMock.mockResolvedValue(jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 2,
      frame_count: 2,
      requested_span_ms: 440,
      span_ms: 4,
      decimation_factor: 1,
      buffered_frames: 2,
      total_frames: 2,
      points: [0, 2].map((timestamp_us) => ({
        timestamp_us,
        end_timestamp_us: timestamp_us,
        channels: [1, 2, 3, 4].map((id) => ({ id, min_uv: 0, max_uv: 1 })),
      })),
    }));
    await expect(fetchSeismic({ spanMs: 440, maxPoints: 220 })).resolves.toMatchObject({ count: 2 });
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('serializes and normalizes a saved session range with an envelope spike', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      object_id: 'OBJ_0001',
      session_id: 'S000001',
      source: 'AD7606',
      complete: true,
      sample_rate_hz: 500,
      channel_count: 4,
      samples_written: 20,
      dropped_frames: 0,
      recording_ms: 40,
      source_frame_count: 20,
      start_frame: 4,
      frame_count: 8,
      count: 2,
      start_timestamp_us: 10000,
      end_timestamp_us: 24000,
      points: [10000, 20000].map((timestamp_us, index) => ({
        timestamp_us,
        channels: [1, 2, 3, 4].map((id) => ({
          id,
          min_uv: id === 1 && index === 0 ? -12000 : 0,
          max_uv: id === 1 && index === 0 ? 18000 : 1,
        })),
      })),
    }));
    vi.stubGlobal('fetch', fetchMock);

    const snapshot = await fetchSavedSeismicSession('OBJ_0001', 'S000001', {
      startFrame: 4,
      frameCount: 8,
      points: 240,
    });
    expect(fetchMock.mock.calls[0][0]).toBe(
      '/api/seismic/session?object_id=OBJ_0001&session_id=S000001&start_frame=4&frame_count=8&points=240',
    );
    expect(snapshot.points[0].channels[0]).toEqual({ id: 1, min_uv: -12000, max_uv: 18000 });

    const lines = envelopePolylinePoints(
      snapshot.points,
      0,
      720,
      120,
      snapshot.start_timestamp_us,
      snapshot.end_timestamp_us,
    );
    expect(lines.min).not.toBe('');
    expect(lines.max).not.toBe('');
    expect(lines.scale_uv).toBe(20700);
  });

  it('serializes the phone GPS payload sent back to the ESP object API', async () => {
    const fetchMock = vi.fn(async (_url: string, init?: RequestInit) => {
      expect(JSON.parse(String(init?.body))).toEqual({
        action: 'set_object_location',
        object_id: 'OBJ_0001',
        latitude: 50.4501,
        longitude: 30.5234,
        accuracy_m: 3.2,
        altitude_m: 179.4,
        altitude_accuracy_m: 5.5,
        captured_at_ms: 123456789,
      });
      return jsonResponse({ ok: true, action: 'set_object_location', object_id: 'OBJ_0001' });
    });
    vi.stubGlobal('fetch', fetchMock);

    await expect(sendControl('set_object_location', {
      object_id: 'OBJ_0001',
      latitude: 50.4501,
      longitude: 30.5234,
      accuracy_m: 3.2,
      altitude_m: 179.4,
      altitude_accuracy_m: 5.5,
      captured_at_ms: 123456789,
    })).resolves.toMatchObject({ ok: true, action: 'set_object_location' });
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0][0]).toBe('/api/control');
  });

  it('keeps saved-session requests behind the shared API serialization gate', async () => {
    let resolveFirst!: (response: Response) => void;
    const fetchMock = vi.fn((url: string) => {
      if (url.includes('S000001')) {
        return new Promise<Response>((resolve) => { resolveFirst = resolve; });
      }
      return Promise.resolve(jsonResponse({
        object_id: 'OBJ_0001', session_id: 'S000002', source: 'AD7606', complete: true,
        sample_rate_hz: 500, channel_count: 4, samples_written: 1, dropped_frames: 0, recording_ms: 2,
        source_frame_count: 1, start_frame: 0, frame_count: 1, count: 1,
        start_timestamp_us: 1, end_timestamp_us: 1,
        points: [{ timestamp_us: 1, channels: [1, 2, 3, 4].map((id) => ({ id, min_uv: 0, max_uv: 0 })) }],
      }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const first = fetchSavedSeismicSession('OBJ_0001', 'S000001');
    const second = fetchSavedSeismicSession('OBJ_0001', 'S000002');
    await Promise.resolve();
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveFirst(jsonResponse({
      object_id: 'OBJ_0001', session_id: 'S000001', source: 'AD7606', complete: true,
      sample_rate_hz: 500, channel_count: 4, samples_written: 1, dropped_frames: 0, recording_ms: 2,
      source_frame_count: 1, start_frame: 0, frame_count: 1, count: 1,
      start_timestamp_us: 1, end_timestamp_us: 1,
      points: [{ timestamp_us: 1, channels: [1, 2, 3, 4].map((id) => ({ id, min_uv: 0, max_uv: 0 })) }],
    }));
    await Promise.all([first, second]);
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('rejects a saved envelope that reverses min and max', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      object_id: 'OBJ_0001', session_id: 'S000001', source: 'AD7606', complete: true,
      sample_rate_hz: 500, channel_count: 4, samples_written: 1, dropped_frames: 0, recording_ms: 2,
      source_frame_count: 1, start_frame: 0, frame_count: 1, count: 1,
      start_timestamp_us: 1, end_timestamp_us: 1,
      points: [{ timestamp_us: 1, channels: [1, 2, 3, 4].map((id) => ({ id, min_uv: 1, max_uv: 0 })) }],
    }));
    vi.stubGlobal('fetch', fetchMock);

    await expect(fetchSavedSeismicSession('OBJ_0001', 'S000001')).rejects.toThrow('invalid envelope range');
  });
});
