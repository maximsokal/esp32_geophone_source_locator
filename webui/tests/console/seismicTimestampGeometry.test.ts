import { describe, expect, it } from 'vitest';
import { normalizeSavedSeismicSnapshot, normalizeSeismicSnapshot } from '../../src/domain/device';
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

function liveBase() {
  return {
    source: 'AD7606',
    sample_rate_hz: 20000,
    count: 2,
    frame_count: 40,
    requested_span_ms: 2,
    span_ms: 2,
    decimation_factor: 20,
    buffered_frames: 80,
    total_frames: 100,
    points: [
      { timestamp_us: 1_000, end_timestamp_us: 2_000, channels: channels(-5) },
      { timestamp_us: 2_100, end_timestamp_us: 3_000, channels: channels(-10) },
    ],
  };
}

describe('seismic timestamp geometry', () => {
  it('normalizes live time-window envelopes with effective-rate and decimation metadata', () => {
    const snapshot = normalizeSeismicSnapshot(liveBase());
    const geometry = envelopePolylinePoints(snapshot.points, 0, 120, 100, 1_000, 3_000);

    expect(snapshot.sample_rate_hz).toBe(20_000);
    expect(snapshot.requested_span_ms).toBe(2);
    expect(snapshot.span_ms).toBe(2);
    expect(snapshot.frame_count).toBe(40);
    expect(snapshot.decimation_factor).toBe(20);
    expect(snapshot.points[0].channels[0]).toEqual({ id: 1, min_uv: -5, max_uv: -4 });
    expect(geometry.min).toContain('M 0.0');
    expect(geometry.min).toContain('L 60.0');
    expect(geometry.max).toContain('L 60.0');
  });

  it('rejects invalid live envelope channel ranges and non-monotonic buckets', () => {
    const invalidRange = liveBase();
    invalidRange.points[0].channels[0] = { id: 1, min_uv: 5, max_uv: -5 };
    expect(() => normalizeSeismicSnapshot(invalidRange)).toThrow(/envelope range/i);

    const invalidTiming = liveBase();
    invalidTiming.points[1].timestamp_us = 900;
    expect(() => normalizeSeismicSnapshot(invalidTiming)).toThrow(/timing/i);
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
