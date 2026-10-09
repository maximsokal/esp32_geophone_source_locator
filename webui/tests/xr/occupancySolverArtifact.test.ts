import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it } from 'vitest';

import {
  decodeOccupancySolverArtifact,
  encodeOccupancySolverArtifact,
  occupancyCrc32,
} from '../../src/xr/occupancySolverArtifact';
import { createProjectFrameRegistration, projectRegistrationSnapshot } from './support/projectFrameRegistration';

/*
 * MAX-162: the compact occupancy artifact is produced here and parsed on the
 * ESP32 by components/scan_occupancy. The golden vector below is shared with
 * the C host test (tests/host/scan_occupancy_test.c) so both sides agree on
 * the exact byte layout. Regenerate with WRITE_GOLDEN_VECTORS=1 after an
 * intentional format change (and bump the schema).
 */
const VECTOR_PATH = resolve(__dirname, '../../../tests/vectors/scan_occupancy_v2.bin');
const LEGACY_VECTOR_PATH = resolve(__dirname, '../../../tests/vectors/scan_occupancy_v1.bin');

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
    frame: 'project_z_up' as const,
    source: 'test',
    geometry_computed_at_ms: 123456,
    sensor_to_scan: [1, 0, 0, 0.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    residual_m: 0.01,
    threshold_m: 0.05,
  },
};

function projectGoldenBytes(): Uint8Array {
  const { occupancyRegistration } = createProjectFrameRegistration();
  return encodeOccupancySolverArtifact(projectRegistrationSnapshot(), {
    frameId: 'scan-crc32:deadbeef',
    createdAtMs: 1700000000000,
    registration: occupancyRegistration,
  });
}

function rewriteHeader(bytes: Uint8Array, update: (header: Record<string, unknown>) => void): Uint8Array {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(8, true);
  const payloadLength = view.getUint32(12, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(20, 20 + headerLength))) as Record<string, unknown>;
  update(header);
  const encodedHeader = new TextEncoder().encode(JSON.stringify(header));
  const result = new Uint8Array(20 + encodedHeader.length + payloadLength);
  result.set(bytes.subarray(0, 20));
  result.set(encodedHeader, 20);
  result.set(bytes.subarray(20 + headerLength), 20 + encodedHeader.length);
  const resultView = new DataView(result.buffer);
  resultView.setUint32(8, encodedHeader.length, true);
  resultView.setUint32(16, occupancyCrc32(result, 20), true);
  return result;
}

type TestVoxel = readonly [number, number, number];

function makeVoxelArtifact(
  voxelSizeJson: string,
  free: readonly TestVoxel[],
  occupied: readonly TestVoxel[],
): Uint8Array {
  const header = new TextEncoder().encode(
    `{"schema":1,"frame_id":"voxel-test","voxel_size_m":${voxelSizeJson},` +
    `"below_surface_m":0,"envelope":null,"free_count":${free.length},` +
    `"occupied_count":${occupied.length},"saturated":false,"created_at_ms":1,"registration":null}`,
  );
  const cells = [...free, ...occupied];
  const result = new Uint8Array(20 + header.length + cells.length * 6);
  result.set(new TextEncoder().encode('GOCCUPY1'), 0);
  const view = new DataView(result.buffer);
  view.setUint32(8, header.length, true);
  view.setUint32(12, cells.length * 6, true);
  result.set(header, 20);
  let offset = 20 + header.length;
  for (const [x, y, z] of cells) {
    view.setInt16(offset, x, true);
    view.setInt16(offset + 2, y, true);
    view.setInt16(offset + 4, z, true);
    offset += 6;
  }
  view.setUint32(16, occupancyCrc32(result, 20), true);
  return result;
}

describe('compact occupancy solver artifact (MAX-162)', () => {
  it.skipIf(process.env.WRITE_GOLDEN_VECTORS !== '1')('writes the registered v2 vector from the production encoder', () => {
    writeFileSync(VECTOR_PATH, projectGoldenBytes());
  });

  it('round-trips FREE/OCCUPIED voxel indices and metadata exactly', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), GOLDEN_OPTIONS);
    const decoded = decodeOccupancySolverArtifact(bytes);
    expect(Array.from(decoded.free)).toEqual([0, 0, 0, 1, -1, 2]);
    expect(Array.from(decoded.occupied)).toEqual([0, 1, -1]);
    expect(decoded.header.voxel_size_m).toBe(0.18);
    expect(decoded.header.below_surface_m).toBe(2);
    expect(decoded.header.registration?.geometry_computed_at_ms).toBe(123456);
    expect(decoded.header.schema).toBe(2);
    expect(decoded.header.registration?.frame).toBe('project_z_up');
    expect(decoded.header.frame_id).toBe('scan-crc32:deadbeef');
  });

  it('matches the v2 golden vector shared with the ESP32 parser', () => {
    expect(Array.from(new Uint8Array(readFileSync(VECTOR_PATH)))).toEqual(Array.from(projectGoldenBytes()));
  });

  it('keeps schema-1 registration readable but explicitly ignored', () => {
    const legacy = decodeOccupancySolverArtifact(new Uint8Array(readFileSync(LEGACY_VECTOR_PATH)));
    expect(legacy.header.schema).toBe(1);
    expect(legacy.header.registration).toBeNull();
    expect(legacy.legacyRegistrationIgnored).toBe(true);

    const withoutRegistration = rewriteHeader(new Uint8Array(readFileSync(LEGACY_VECTOR_PATH)), (header) => {
      header.registration = null;
    });
    const unregistered = decodeOccupancySolverArtifact(withoutRegistration);
    expect(unregistered.header.registration).toBeNull();
    expect(unregistered.legacyRegistrationIgnored).toBe(false);
  });

  it('rejects unknown schemas and registration frames', () => {
    const bytes = projectGoldenBytes();
    expect(() => decodeOccupancySolverArtifact(rewriteHeader(bytes, (header) => {
      header.schema = 9;
    }))).toThrow(/schema/i);
    expect(() => decodeOccupancySolverArtifact(rewriteHeader(bytes, (header) => {
      (header.registration as Record<string, unknown>).frame = 'unknown';
    }))).toThrow(/frame/i);
  });

  it('rejects reflected registrations on both artifact write and read paths', () => {
    const reflected = [-1, 0, 0, 0.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    const registration = { ...GOLDEN_OPTIONS.registration, sensor_to_scan: reflected };
    expect(() => encodeOccupancySolverArtifact(goldenSnapshot(), { ...GOLDEN_OPTIONS, registration })).toThrow('reflected_transform');

    const bytes = projectGoldenBytes();
    expect(() => decodeOccupancySolverArtifact(rewriteHeader(bytes, (header) => {
      (header.registration as Record<string, unknown>).sensor_to_scan = reflected;
    }))).toThrow('reflected_transform');
  });

  it('rejects corruption instead of reading garbage', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), GOLDEN_OPTIONS);
    bytes[bytes.length - 1] ^= 0xff;
    expect(() => decodeOccupancySolverArtifact(bytes)).toThrow(/checksum/);
  });

  it('rejects duplicate FREE, OCCUPIED, and cross-state voxel keys', () => {
    const valid = decodeOccupancySolverArtifact(makeVoxelArtifact(
      '0.5', [[0, 0, 0], [1, -1, 2]], [[0, 1, -1]],
    ));
    expect(valid.header.voxel_size_m).toBe(0.5);
    expect(valid.free).toHaveLength(6);
    expect(valid.occupied).toHaveLength(3);

    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact(
      '0.18', [[4, -2, 7], [4, -2, 7]], [],
    ))).toThrow(/duplicate/i);
    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact(
      '0.18', [], [[-3, 8, 1], [-3, 8, 1]],
    ))).toThrow(/duplicate/i);
    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact(
      '0.18', [[11, -9, 2]], [[11, -9, 2]],
    ))).toThrow(/duplicate/i);
  });

  it('rejects non-finite, non-positive, and absurd voxel sizes', () => {
    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact('1e999', [], []))).toThrow(/voxel size/i);
    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact('0', [], []))).toThrow(/voxel size/i);
    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact('-0.18', [], []))).toThrow(/voxel size/i);
    expect(() => decodeOccupancySolverArtifact(makeVoxelArtifact('1e308', [], []))).toThrow(/voxel size/i);
  });

  it('writes threshold_m and refuses a registration whose residual exceeds it or that lacks one', () => {
    const decoded = decodeOccupancySolverArtifact(encodeOccupancySolverArtifact(goldenSnapshot(), GOLDEN_OPTIONS));
    expect(decoded.header.registration?.threshold_m).toBe(0.05);
    const withReg = (patch: Record<string, unknown>) => ({ ...GOLDEN_OPTIONS, registration: { ...GOLDEN_OPTIONS.registration, ...patch } });
    expect(() => encodeOccupancySolverArtifact(goldenSnapshot(), withReg({ residual_m: 0.3 }))).toThrow(/threshold/i);
    expect(() => encodeOccupancySolverArtifact(goldenSnapshot(), withReg({ threshold_m: undefined }))).toThrow(/threshold/i);
    expect(() => encodeOccupancySolverArtifact(goldenSnapshot(), withReg({ residual_m: 0.05 }))).not.toThrow();
  });

  it('keeps registration null until a real registration exists', () => {
    const bytes = encodeOccupancySolverArtifact(goldenSnapshot(), { frameId: 'f', createdAtMs: 1 });
    expect(decodeOccupancySolverArtifact(bytes).header.registration).toBeNull();
  });
});
