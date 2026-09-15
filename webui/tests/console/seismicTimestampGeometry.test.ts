import { describe, expect, it } from 'vitest';
import { normalizeSavedSeismicSnapshot } from '../../src/domain/device';
import { polylinePoints } from '../../src/features/seismic/LiveSeismicPanel';
import { envelopePolylinePoints } from '../../src/features/seismic/savedWaveform';

function channels(value: number) {
  return [1, 2, 3, 4].map((id) => ({ id, min_uv: value, max_uv: value + 1 }));
}

function savedBase() {
  return {
    object_id: 'OBJ_0001',
    session_id: 'S000001',
    source: 'AD7606',
    sample_rate_hz: 500,
    channel_count: 4,
    samples_written: 2,
    dropped_frames: 0,
    recording_ms: 4,
    source_frame_count: 2,
    start_frame: 0,
    frame_count: 2,
    count: 2,
    start_timestamp_us: 2_000,
    end_timestamp_us: 10_000,
    points: [
      { timestamp_us: 2_000, end_timestamp_us: 4_000, channels: channels(0) },
      { timestamp_us: 8_000, end_timestamp_us: 10_000, channels: channels(1) },
    ],
  };
}

describe('seismic timestamp geometry', () => {
  it('uses real live timestamps instead of sample index', () => {
    const points = polylinePoints([0, 0, 0, 0, 0], [0, 2, 4, 10, 12], 120, 100);
    expect(points.split(' ').map((point) => point.split(',')[0])).toEqual([
      '0.0', '20.0', '40.0', '100.0', '120.0',
    ]);
  });

  it('handles equal live timestamps deterministically and rejects non-monotonic input', () => {
    expect(polylinePoints([1, 2], [5, 5], 120, 100).split(' ').map((point) => point.split(',')[0]))
      .toEqual(['60.0', '60.0']);
    expect(polylinePoints([1, 2, 3], [0, 4, 2], 120, 100)).toBe('');
    expect(polylinePoints([1, 2], [0], 120, 100)).toBe('');
  });

  it('renders saved buckets across their real horizontal extent and leaves timestamp gaps blank', () => {
    const geometry = envelopePolylinePoints([
      { timestamp_us: 0, end_timestamp_us: 4_000, channels: channels(1) },
      { timestamp_us: 10_000, end_timestamp_us: 12_000, channels: channels(2) },
    ], 0, 120, 100, 0, 12_000);

    expect(geometry.min).toContain('M 0.0');
    expect(geometry.min).toContain('L 40.0');
    expect(geometry.min).toContain('M 100.0');
    expect(geometry.min).toContain('L 120.0');
    expect(geometry.min).not.toContain('L 100.0');
  });

  it('keeps legacy saved complete sessions readable as cadence-unknown and point-instantaneous', () => {
    const snapshot = normalizeSavedSeismicSnapshot({
      object_id: 'OBJ_0001',
      session_id: 'S000001',
      source: 'AD7606',
      complete: true,
      sample_rate_hz: 500,
      channel_count: 4,
      samples_written: 1,
      dropped_frames: 0,
      recording_ms: 2,
      source_frame_count: 1,
      start_frame: 0,
      frame_count: 1,
      count: 1,
      start_timestamp_us: 2_000,
      end_timestamp_us: 2_000,
      points: [{ timestamp_us: 2_000, channels: channels(0) }],
    });

    expect(snapshot.status).toBe('complete');
    expect(snapshot.complete).toBe(true);
    expect(snapshot.readable).toBe(true);
    expect(snapshot.recovered).toBe(false);
    expect(snapshot.cadence.available).toBe(false);
    expect(snapshot.points[0].end_timestamp_us).toBe(2_000);
  });

  it('accepts readable terminal error/recovered sessions without coercing them to complete', () => {
    const errored = normalizeSavedSeismicSnapshot({
      ...savedBase(),
      status: 'error',
      complete: false,
      readable: true,
      recovered: false,
      dropped_frames_available: true,
      recording_ms_available: true,
    });
    expect(errored.status).toBe('error');
    expect(errored.complete).toBe(false);
    expect(errored.readable).toBe(true);
    expect(errored.recovered).toBe(false);

    const recovered = normalizeSavedSeismicSnapshot({
      ...savedBase(),
      status: 'recovered_error',
      complete: false,
      readable: true,
      recovered: true,
      dropped_frames_available: false,
      recording_ms_available: false,
    });
    expect(recovered.status).toBe('recovered_error');
    expect(recovered.complete).toBe(false);
    expect(recovered.readable).toBe(true);
    expect(recovered.recovered).toBe(true);
    expect(recovered.dropped_frames_available).toBe(false);
    expect(recovered.recording_ms_available).toBe(false);
  });

  it('rejects arbitrary nonterminal incomplete payloads and inconsistent terminal flags', () => {
    expect(() => normalizeSavedSeismicSnapshot({
      ...savedBase(),
      complete: false,
    })).toThrow(/terminal\/readable/i);

    expect(() => normalizeSavedSeismicSnapshot({
      ...savedBase(),
      status: 'recovered_error',
      complete: false,
      readable: true,
      recovered: false,
    })).toThrow(/recovery state/i);

    expect(() => normalizeSavedSeismicSnapshot({
      ...savedBase(),
      status: 'error',
      complete: true,
      readable: true,
      recovered: false,
    })).toThrow(/completion\/status/i);
  });

  it('validates cadence evidence and saved interval monotonicity', () => {
    const base = {
      ...savedBase(),
      complete: true,
      cadence: { available: true, timer_ticks: 3, conversion_attempts: 2, collapsed_periods: 1 },
    };

    const valid = normalizeSavedSeismicSnapshot(base);
    expect(valid.cadence.collapsed_periods).toBe(1);

    expect(() => normalizeSavedSeismicSnapshot({
      ...base,
      cadence: { available: true, timer_ticks: 3, conversion_attempts: 3, collapsed_periods: 1 },
    })).toThrow(/cadence counters/i);

    expect(() => normalizeSavedSeismicSnapshot({
      ...base,
      points: [
        { timestamp_us: 8_000, end_timestamp_us: 10_000, channels: channels(0) },
        { timestamp_us: 2_000, end_timestamp_us: 4_000, channels: channels(1) },
      ],
    })).toThrow(/timing geometry/i);
  });
});
