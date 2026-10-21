import { beforeEach, describe, expect, it, vi } from 'vitest';
import { fetchSavedSeismicSession } from '../../src/api/deviceApi';
import {
  fetchResultSegment,
  resultViewArrivalMarkerOffset,
  resultViewSegmentWindow,
  resultViewSourceDistances,
  type ResultSegmentPick,
} from '../../src/domain/resultSegment';
import type {
  LocalizationRunPosition,
  LocalizationSessionPodGeometry,
} from '../../src/api/deviceApi';
import type { SavedSeismicSnapshot } from '../../src/domain/device';
import fixtureJson from '../../../tests/fixtures/result_view_vectors.json';

vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../src/api/deviceApi')>(),
  fetchSavedSeismicSession: vi.fn(),
}));

const fixture = fixtureJson as { windows: number[][]; distances: number[][] };
const windowNames = ['middle', 'start edge', 'end edge', 'record-spanning', 'single accepted pick', 'no accepted picks', 'rejected pick ignored', 'record shorter than minimum', 'sample-rate conversion', 'fractional sample conversion'];

function picksFromVector(vector: number[]): ResultSegmentPick[] {
  const pickCount = vector[1];
  const statuses = ['accepted', 'rejected_no_trigger', 'rejected_low_snr', 'rejected_inconsistent'] as const;
  return Array.from({ length: pickCount }, (_, index) => {
    const column = 2 + index * 3;
    return {
      arrival_sample_index: vector[column],
      status: statuses[vector[column + 1] as 0 | 1 | 2 | 3],
      snr: vector[column + 2],
    };
  });
}

describe('result segment window golden vectors', () => {
  it.each(fixture.windows.map((vector, index) => [windowNames[index], vector] as const))(
    '%s',
    (_name, vector) => {
      const result = resultViewSegmentWindow(
        picksFromVector(vector), vector[14], vector[15], vector[16], vector[17],
      );
      expect(result.available).toBe(vector[18] === 1);
      expect(result.start_frame).toBe(vector[19]);
      expect(result.frame_count).toBe(vector[20]);
    },
  );

  it('rejects invalid metadata and accepted picks outside the recording', () => {
    expect(() => resultViewSegmentWindow([], 0, 100)).toThrow(RangeError);
    expect(() => resultViewSegmentWindow([
      { arrival_sample_index: 100, status: 'accepted', snr: 7 },
    ], 100, 100)).toThrow(RangeError);
  });
});

describe('arrival markers', () => {
  const window = { available: true, start_frame: 100, frame_count: 20 };

  it('returns the accepted marker offset within the window', () => {
    expect(resultViewArrivalMarkerOffset(
      { arrival_sample_index: 110, status: 'accepted', snr: 9 }, window,
    )).toBe(10);
  });

  it('omits rejected, outside, and unavailable markers', () => {
    expect(resultViewArrivalMarkerOffset(
      { arrival_sample_index: 110, status: 'rejected_low_snr', snr: 1 }, window,
    )).toBeNull();
    expect(resultViewArrivalMarkerOffset(
      { arrival_sample_index: 120, status: 'accepted', snr: 9 }, window,
    )).toBeNull();
    expect(resultViewArrivalMarkerOffset(
      { arrival_sample_index: 110, status: 'accepted', snr: 9 }, { ...window, available: false },
    )).toBeNull();
  });
});

describe('source distances golden vectors', () => {
  it.each(fixture.distances.map((vector) => [vector[0], vector] as const))(
    'distance vector %s',
    (_id, vector) => {
      const podCount = vector[1];
      const pods: LocalizationSessionPodGeometry[] = Array.from({ length: podCount }, (_, index) => {
        const column = 5 + index * 4;
        return {
          pod: index + 1,
          computed: vector[column] === 1,
          leveled: false,
          x_mm: vector[column + 1],
          y_mm: vector[column + 2],
          z_mm: vector[column + 3],
        };
      });
      const source: LocalizationRunPosition = { x_m: vector[2], y_m: vector[3], z_m: vector[4] };
      const distances = resultViewSourceDistances(source, pods);
      expect(distances).toHaveLength(podCount);
      for (let index = 0; index < podCount; index += 1) {
        const expectedColumn = 21 + index * 2;
        if (vector[expectedColumn] === 0) {
          expect(distances[index]).toBeNull();
        } else {
          expect(distances[index]).toBeCloseTo(vector[expectedColumn + 1], 6);
        }
      }
    },
  );

  it('rejects more pods than the frozen geometry supports', () => {
    const source = { x_m: 0, y_m: 0, z_m: 0 };
    const pods = Array.from({ length: 5 }, (_, pod) => ({
      pod: pod + 1, computed: false, leveled: false,
    }));
    expect(() => resultViewSourceDistances(source, pods)).toThrow(RangeError);
  });
});

describe('saved result segment fetch', () => {
  beforeEach(() => vi.mocked(fetchSavedSeismicSession).mockReset());

  it('passes the computed window to the existing saved-session API', async () => {
    const session = { object_id: 'object', session_id: 'session' } as SavedSeismicSnapshot;
    vi.mocked(fetchSavedSeismicSession).mockResolvedValue(session);
    const result = await fetchResultSegment(
      'object', 'session', [{ arrival_sample_index: 200, status: 'accepted', snr: 9 }], 100, 500,
    );
    expect(fetchSavedSeismicSession).toHaveBeenCalledWith('object', 'session', { startFrame: 125, frameCount: 200 });
    expect(result?.session).toBe(session);
  });

  it('does not fetch a segment when no pick was accepted', async () => {
    const result = await fetchResultSegment(
      'object', 'session', [{ arrival_sample_index: 5, status: 'rejected_no_trigger', snr: 0 }], 100, 500,
    );
    expect(result).toBeNull();
    expect(fetchSavedSeismicSession).not.toHaveBeenCalled();
  });
});
