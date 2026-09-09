import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  fetchDeviceStatus,
  fetchSavedSeismicSession,
  fetchSeismic,
  sendControl,
} from '../../src/api/deviceApi';
import { polylinePoints } from '../../src/features/seismic/LiveSeismicPanel';
import { envelopePolylinePoints } from '../../src/features/seismic/SavedSeismicViewer';

function jsonResponse(body: unknown, status = 200, statusText = 'OK'): Response {
  return new Response(JSON.stringify(body), {
    status,
    statusText,
    headers: { 'Content-Type': 'application/json' },
  });
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('serialized ESP API traffic', () => {
  it('does not start a control POST while a status fetch is still in flight', async () => {
    let resolveStatus!: (response: Response) => void;
    const fetchMock = vi.fn((url: string) => {
      if (url === '/api/status') {
        return new Promise<Response>((resolve) => { resolveStatus = resolve; });
      }
      return Promise.resolve(jsonResponse({ ok: true, action: 'activate_object' }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const status = fetchDeviceStatus(false);
    await Promise.resolve();
    expect(fetchMock).toHaveBeenCalledTimes(1);

    const control = sendControl('activate_object', { object_id: 'OBJ_0001' });
    await Promise.resolve();
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveStatus(jsonResponse({ system: { state: 'NOT_READY' } }));
    await status;
    await control;

    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls[1][0]).toBe('/api/control');
  });

  it('coalesces live seismic requests while one sample fetch is pending', async () => {
    let resolveSeismic!: (response: Response) => void;
    const fetchMock = vi.fn(() => new Promise<Response>((resolve) => { resolveSeismic = resolve; }));
    vi.stubGlobal('fetch', fetchMock);

    const first = fetchSeismic(220);
    const second = fetchSeismic(220);
    await Promise.resolve();

    expect(first).toBe(second);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    resolveSeismic(jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 0,
      buffered_frames: 0,
      total_frames: 0,
      timestamps_ms: [],
      channels: [1, 2, 3, 4].map((id) => ({ id, samples_uv: [] })),
    }));
    await Promise.all([first, second]);
  });

  it('carries a signed API impulse through polling normalization into waveform points', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 3,
      buffered_frames: 3,
      total_frames: 103,
      timestamps_ms: [1000, 1002, 1004],
      channels: [1, 2, 3, 4].map((id) => ({
        id,
        samples_uv: id === 1 ? [-100, 100, -50] : [0, 0, 0],
      })),
    }));
    vi.stubGlobal('fetch', fetchMock);

    const snapshot = await fetchSeismic(220);
    const points = polylinePoints(snapshot.channels[0].samples_uv, 720, 120);

    expect(snapshot.channels[0].samples_uv).toEqual([-100, 100, -50]);
    expect(points.split(' ')).toHaveLength(3);
    expect(points).toContain('0.0,112.8');
    expect(points).toContain('360.0,7.2');
    expect(points).toContain('720.0,86.4');
  });

  it('rejects a malformed seismic channel before it reaches the renderer', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 1,
      buffered_frames: 1,
      total_frames: 1,
      timestamps_ms: [1000],
      channels: [1, 2, 3, 8].map((id) => ({ id, samples_uv: [1] })),
    }));
    vi.stubGlobal('fetch', fetchMock);

    await expect(fetchSeismic(220)).rejects.toThrow('invalid channel shape');

    fetchMock.mockResolvedValue(jsonResponse({
      source: 'AD7606',
      sample_rate_hz: 500,
      count: 2,
      buffered_frames: 2,
      total_frames: 2,
      timestamps_ms: [1000, 1002],
      channels: [1, 2, 3, 4].map((id) => ({ id, samples_uv: [0, 1] })),
    }));
    await expect(fetchSeismic(220)).resolves.toMatchObject({ count: 2 });
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

    const lines = envelopePolylinePoints(snapshot.points, 0, 720, 120);
    expect(lines.min).not.toBe('');
    expect(lines.max).not.toBe('');
    expect(lines.scale_uv).toBe(20700);
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
