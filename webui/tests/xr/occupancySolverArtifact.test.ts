import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it } from 'vitest';

import {
  decodeOccupancySolverArtifact,
  encodeOccupancySolverArtifact,
} from '../../src/xr/occupancySolverArtifact';

/*
 * MAX-162: the compact occupancy artifact is produced here and parsed on the
 * ESP32 by components/scan_occupancy. The golden vector below is shared with
 * the C host test (tests/host/scan_occupancy_test.c) so both sides agree on
 * the exact byte layout. Regenerate with UPDATE_VECTORS=1 after an
 * intentional format change (and bump the schema).
 */
const VECTOR_PATH = resolve(__dirname, '../../../tests/vectors/scan_occupancy_v1.bin');

function goldenSnapshot() {
  return {
    // voxel 0.18 m: (0,0,0) and (1,-1,2) FREE, (0,1,-1) OCCUPIED
    freeAirPositions: new Float32Array([0.09, 0.09, 0.09, 0.27, -0.09, 0.45]),
    occupiedSurfacePositions: new Float32Array([0.09, 0.27, -0.09]),
    occupancyEnvelopeBounds: { min: [-1, -1, -0.5] as const, max: [1, 1, 0.5] as const },
    occupancySolverCandidateBelowSurfaceMeters: 2,
    occupancyCellsSaturated: false,
    occupancyFineVoxelSizeMeters: 0.18,
    occupancyVoxelSizeMeters: 0.18,
  };
}

const GOLDEN_OPTIONS = {
  frameId: 'scan-crc32:deadbeef',
  createdAtMs: 1700000000000,
  registration: {
    source: 'test',
    geometry_computed_at_ms: 123456,
    sensor_to_scan: [1, 0, 0, 0.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    residual_m: 0.01,
  },
};

describe('compact occupancy solver artifact (MAX-162)', () => {
  it('round-trips FREE/OCCUPIED voxel indices and metadata exactly', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), GOLDEN_OPTIONS);
    const decoded = decodeOccupancySolverArtifact(bytes);
    expect(Array.from(decoded.free)).toEqual([0, 0, 0, 1, -1, 2]);
    expect(Array.from(decoded.occupied)).toEqual([0, 1, -1]);
    expect(decoded.header.voxel_size_m).toBe(0.18);
    expect(decoded.header.below_surface_m).toBe(2);
    expect(decoded.header.registration?.geometry_computed_at_ms).toBe(123456);
    expect(decoded.header.frame_id).toBe('scan-crc32:deadbeef');
  });

  it('matches the golden vector shared with the ESP32 parser', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), GOLDEN_OPTIONS);
    if (process.env.UPDATE_VECTORS === '1' || !existsSync(VECTOR_PATH)) {
      writeFileSync(VECTOR_PATH, bytes);
    }
    expect(Array.from(new Uint8Array(readFileSync(VECTOR_PATH)))).toEqual(Array.from(bytes));
  });

  it('rejects corruption instead of reading garbage', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), GOLDEN_OPTIONS);
    bytes[bytes.length - 1] ^= 0xff;
    expect(() => decodeOccupancySolverArtifact(bytes)).toThrow(/checksum/);
  });

  it('keeps registration null until a real registration exists', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), { frameId: 'f', createdAtMs: 1 });
    expect(decodeOccupancySolverArtifact(bytes).header.registration).toBeNull();
  });
});
