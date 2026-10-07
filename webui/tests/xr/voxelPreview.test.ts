import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import {
  encodeOccupancySolverArtifact,
  occupancyCrc32,
  occupancyFrameIdForScan,
} from '../../src/xr/occupancySolverArtifact';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { localFloorMatrixToProject } from '../../src/xr/projectFrame';
import {
  buildVoxelPreviewArtifact,
  buildVoxelPreviewGrid,
  decodeVoxelPreviewArtifact,
  encodeVoxelPreviewArtifact,
  DEFAULT_MAX_SURFACE_VOXELS,
  MAX_VOXEL_PREVIEW_BYTES,
  type VoxelPreviewArtifact,
} from '../../src/xr/voxelPreview';

const scanA = new Uint8Array([0x11, 0x22, 0x33, 0x44]);
const PREVIEW_VECTOR_PATH = resolve(__dirname, '../../../tests/vectors/voxel_preview_v2.bin');

function snapshot(cells: readonly (readonly [number, number, number])[], voxelSize = 1): ScanPointCloudSnapshot {
  const occupiedSurfacePositions = new Float32Array(cells.flatMap(([x, y, z]) => [x + 0.5, y + 0.5, z + 0.5]));
  return {
    occupiedSurfacePositions,
    occupancyFineVoxelSizeMeters: voxelSize,
    occupancyVoxelSizeMeters: voxelSize,
  } as ScanPointCloudSnapshot;
}

function cube(size: number): Array<[number, number, number]> {
  const result: Array<[number, number, number]> = [];
  for (let z = 0; z < size; z += 1) for (let y = 0; y < size; y += 1) for (let x = 0; x < size; x += 1) result.push([x, y, z]);
  return result;
}

function fixCrc(bytes: Uint8Array): Uint8Array {
  const result = bytes.slice();
  new DataView(result.buffer).setUint32(result.length - 4, occupancyCrc32(result.subarray(0, result.length - 4)), true);
  return result;
}

function registeredArtifact(): VoxelPreviewArtifact {
  return {
    sourceScanCrc32: occupancyCrc32(scanA),
    geometryComputedAtMs: 123456n,
    registration: {
      geometry_computed_at_ms: 123456,
      sensor_to_scan: [1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3, 0, 0, 0, 1],
    },
    origin: [-1, 2, 0],
    voxelSizeMeters: 0.25,
    dimensions: [3, 2, 1],
    voxelCount: 2,
    indexWidth: 2,
    indices: new Uint32Array([0, 5]),
    faceMasks: new Uint8Array([0x3f, 0x15]),
    coarseningSteps: 2,
  };
}

function smokeRegistration() {
  return {
    frame: 'webxr_local_floor' as const,
    source: 'test',
    geometry_computed_at_ms: 123456,
    sensor_to_scan: [1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1, 3, 0, 0, 0, 1],
    residual_m: 0,
  };
}

function projectSmokePreviewBytes(): Uint8Array {
  return encodeVoxelPreviewArtifact({
    sourceScanCrc32: occupancyCrc32(scanA),
    geometryComputedAtMs: 123456n,
    registration: {
      geometry_computed_at_ms: 123456,
      sensor_to_scan: localFloorMatrixToProject(smokeRegistration().sensor_to_scan),
    },
    origin: [-0.5, -3.5, 1.5],
    voxelSizeMeters: 1,
    dimensions: [1, 1, 1],
    voxelCount: 1,
    indexWidth: 2,
    indices: new Uint32Array([0]),
    faceMasks: new Uint8Array([0x3f]),
    coarseningSteps: 0,
  });
}

describe('voxel preview grid and preview-v1/v2 format', () => {
  it.skipIf(process.env.WRITE_GOLDEN_VECTORS !== '1')('writes the registered v2 vector from the production encoder', () => {
    writeFileSync(PREVIEW_VECTOR_PATH, projectSmokePreviewBytes());
  });

  it('matches the registered v2 vector consumed by the C parser tests', () => {
    expect(Array.from(new Uint8Array(readFileSync(PREVIEW_VECTOR_PATH)))).toEqual(Array.from(projectSmokePreviewBytes()));
  });

  it('decodes the shared golden vector used by the C parser test', () => {
    const source = readFileSync(new URL('../../../components/scan_preview/tests/preview_v1_golden_bytes.inc', import.meta.url), 'utf8');
    const bytes = Uint8Array.from([...source.matchAll(/0x([0-9a-f]{2})/gi)].map((match) => Number.parseInt(match[1], 16)));
    const decoded = decodeVoxelPreviewArtifact(bytes);
    expect(decoded).toMatchObject({
      sourceScanCrc32: 0x12345678,
      voxelSizeMeters: 0.5,
      dimensions: [2, 2, 1],
      voxelCount: 1,
      coarseningSteps: 3,
    });
    expect([...decoded.indices]).toEqual([3]);
    expect([...decoded.faceMasks]).toEqual([0x3f]);
  });

  it('stores only the shell of a solid box', () => {
    const grid = buildVoxelPreviewGrid(snapshot(cube(3)), scanA);
    expect(grid.indices).toHaveLength(26);
    expect([...grid.faceMasks].every((mask) => mask !== 0)).toBe(true);
    expect(grid.dimensions).toEqual([3, 3, 3]);
    expect(DEFAULT_MAX_SURFACE_VOXELS).toBe(3000);
    expect(buildVoxelPreviewArtifact(snapshot(cube(3)), scanA).byteLength).toBe(136);
  });

  it('keeps a one-voxel-thick plane and disconnected occupied parts', () => {
    const plane = cube(3).filter(([, , z]) => z === 0);
    expect(buildVoxelPreviewGrid(snapshot(plane), scanA).indices).toHaveLength(9);
    expect(buildVoxelPreviewGrid(snapshot([[0, 0, 0], [10, 0, 0]]), scanA).indices).toHaveLength(2);
  });

  it('doubles voxel size until the exact requested surface budget fits', () => {
    const grid = buildVoxelPreviewGrid(snapshot(cube(3)), scanA, { maxSurfaceVoxels: 1 });
    expect(grid.coarseningSteps).toBe(2);
    expect(grid.voxelSizeMeters).toBe(4);
    expect(grid.indices).toHaveLength(1);
  });

  it('prefers matching occupancy cells and ignores a mismatched occupancy artifact', () => {
    const cells = cube(3);
    const fullSnapshot = {
      ...snapshot(cells),
      freeAirPositions: new Float32Array(),
      occupancyEnvelopeBounds: null,
      occupancySolverCandidateBelowSurfaceMeters: 0,
      occupancyCellsSaturated: false,
    } as unknown as Parameters<typeof buildVoxelPreviewArtifact>[0];
    const matching = encodeOccupancySolverArtifact(fullSnapshot, {
      frameId: occupancyFrameIdForScan(scanA),
      createdAtMs: 1,
    });
    const sparseSnapshot = snapshot([[0, 0, 0]]);
    expect(buildVoxelPreviewGrid(sparseSnapshot, scanA).indices).toHaveLength(1);
    expect(buildVoxelPreviewGrid(sparseSnapshot, scanA, { occupancyArtifact: matching }).indices).toHaveLength(26);

    const stale = encodeOccupancySolverArtifact(fullSnapshot, { frameId: 'scan-crc32:00000000', createdAtMs: 1 });
    expect(buildVoxelPreviewGrid(sparseSnapshot, scanA, { occupancyArtifact: stale }).indices).toHaveLength(1);
  });

  it('round-trips the registered header, final voxel size, and coarsening count', () => {
    const encoded = encodeVoxelPreviewArtifact(registeredArtifact());
    expect(new DataView(encoded.buffer).getUint16(8, true)).toBe(2);
    const decoded = decodeVoxelPreviewArtifact(encoded, scanA);
    expect(decoded).toMatchObject({
      sourceScanCrc32: occupancyCrc32(scanA),
      geometryComputedAtMs: 123456n,
      voxelSizeMeters: 0.25,
      dimensions: [3, 2, 1],
      coarseningSteps: 2,
      registration: { geometry_computed_at_ms: 123456 },
    });
    expect(decoded.indices).toEqual(new Uint32Array([0, 5]));
    expect(decoded.faceMasks).toEqual(new Uint8Array([0x3f, 0x15]));
  });

  it('converts local-floor registration before writing v2 and drops registered v1 transforms', () => {
    const encoded = buildVoxelPreviewArtifact(snapshot([[0, 0, 0]]), scanA, { registration: smokeRegistration() });
    const decoded = decodeVoxelPreviewArtifact(encoded);
    expect(decoded.registration?.sensor_to_scan).toEqual([
      1, 0, 0, 0,
      0, 0, -1, -3,
      0, 1, 0, 2,
      0, 0, 0, 1,
    ]);

    const legacyRegistered = encoded.slice();
    new DataView(legacyRegistered.buffer).setUint16(8, 1, true);
    const repaired = fixCrc(legacyRegistered);
    const legacy = decodeVoxelPreviewArtifact(repaired);
    expect(legacy.registration).toBeNull();
    expect(legacy.geometryComputedAtMs).toBe(0n);
  });

  it('uses u32 indices when the grid volume exceeds 65536 cells', () => {
    const artifact = {
      ...registeredArtifact(),
      registration: null,
      geometryComputedAtMs: 0n,
      dimensions: [257, 257, 1] as [number, number, number],
      voxelCount: 2,
      indexWidth: 4 as const,
      indices: new Uint32Array([0, 66048]),
      faceMasks: new Uint8Array([1, 2]),
    };
    expect(decodeVoxelPreviewArtifact(encodeVoxelPreviewArtifact(artifact)).indexWidth).toBe(4);
  });

  it('rejects corrupt magic, version, truncation, counts, indices, masks, order, and CRC', () => {
    const good = encodeVoxelPreviewArtifact(registeredArtifact());
    const badMagic = good.slice(); badMagic[0] ^= 0xff;
    const badVersion = good.slice(); new DataView(badVersion.buffer).setUint16(8, 3, true);
    const badCount = good.slice(); new DataView(badCount.buffer).setUint32(48, 0xffffffff, true);
    const badIndex = good.slice(); new DataView(badIndex.buffer).setUint16(118, 6, true);
    const badMask = good.slice(); badMask[118 + 2 * 2] = 0xc0;
    const cases = [badMagic, badVersion, good.slice(0, -1), fixCrc(badCount), fixCrc(badIndex), fixCrc(badMask)];
    for (const bytes of cases) expect(() => decodeVoxelPreviewArtifact(bytes)).toThrow();

    const solid = encodeVoxelPreviewArtifact({
      ...registeredArtifact(), registration: null, geometryComputedAtMs: 0n,
      dimensions: [3, 1, 1], voxelCount: 3, indexWidth: 2,
      indices: new Uint32Array([0, 1, 2]), faceMasks: new Uint8Array([3, 3, 3]),
    });
    const duplicate = solid.slice(); new DataView(duplicate.buffer).setUint16(56, 0, true);
    const unsorted = solid.slice(); new DataView(unsorted.buffer).setUint16(56, 2, true);
    expect(() => decodeVoxelPreviewArtifact(fixCrc(duplicate))).toThrow(/sorted|duplicated/);
    expect(() => decodeVoxelPreviewArtifact(fixCrc(unsorted))).toThrow(/sorted|duplicated/);
    const badCrc = good.slice(); badCrc[badCrc.length - 1] ^= 1;
    expect(() => decodeVoxelPreviewArtifact(badCrc)).toThrow(/CRC/);
  });

  it('detects a preview generated from a different saved scan CRC', () => {
    const encoded = buildVoxelPreviewArtifact(snapshot([[0, 0, 0]]), scanA);
    expect(() => decodeVoxelPreviewArtifact(encoded, new Uint8Array([9, 8, 7]))).toThrow(/stale/);
  });

  it('keeps the 64 KiB encoded limit', () => {
    const bytes = encodeVoxelPreviewArtifact(registeredArtifact());
    expect(bytes.byteLength).toBeLessThan(MAX_VOXEL_PREVIEW_BYTES);
  });
});
