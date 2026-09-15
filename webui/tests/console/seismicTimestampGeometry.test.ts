import { describe, expect, it } from 'vitest';
import { normalizeSavedSeismicSnapshot } from '../../src/domain/device';
import { polylinePoints } from '../../src/features/seismic/LiveSeismicPanel';
import { envelopePolylinePoints } from '../../src/features/seismic/savedWaveform';

function channels(value: number) {
  return [1, 2, 3, 4].map((id) => ({ id, min_uv: value, max_uv: value + 1 }));
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

  it('keeps legacy saved sessions readable as cadence-unknown and point-instantaneous', () => {
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

    expect(snapshot.cadence.available).toBe(false);
    expect(snapshot.points[0].end_timestamp_us).toBe(2_000);
  });

  it('validates cadence evidence and saved interval monotonicity', () => {
    const base = {
      object_id: 'OBJ_0001',
      session_id: 'S000001',
      source: 'AD7606',
      complete: true,
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
      cadence: { available: true, timer_ticks: 3, conversion_attempts: 2, collapsed_periods: 1 },
    };

    const valid = normalizeSavedSeismicSnapshot({
      ...base,
      points: [
        { timestamp_us: 2_000, end_timestamp_us: 4_000, channels: channels(0) },
        { timestamp_us: 8_000, end_timestamp_us: 10_000, channels: channels(1) },
      ],
    });
    expect(valid.cadence.collapsed_periods).toBe(1);

    expect(() => normalizeSavedSeismicSnapshot({
      ...base,
      cadence: { available: true, timer_ticks: 3, conversion_attempts: 3, collapsed_periods: 1 },
      points: valid.points,
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
