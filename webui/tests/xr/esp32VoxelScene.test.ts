import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { buildEsp32VoxelGeometry, decodeVoxelCell, exposedFaceCount, type Esp32VoxelGeometry } from '../../src/xr/esp32VoxelScene';
import { decodeVoxelPreviewArtifact, type VoxelPreviewArtifact } from '../../src/xr/voxelPreview';

const BIT = { POS_X: 1, NEG_X: 2, POS_Y: 4, NEG_Y: 8, POS_Z: 16, NEG_Z: 32 } as const;

function artifact(overrides: Partial<VoxelPreviewArtifact>): VoxelPreviewArtifact {
  const indices = overrides.indices ?? new Uint32Array([0]);
  return {
    sourceScanCrc32: 1,
    geometryComputedAtMs: 0n,
    registration: null,
    origin: [0, 0, 0],
    voxelSizeMeters: 0.25,
    dimensions: [1, 1, 1],
    voxelCount: indices.length,
    indexWidth: 2,
    indices,
    faceMasks: new Uint8Array(indices.length).fill(63),
    coarseningSteps: 0,
    ...overrides,
  };
}

/** -0 and +0 are the same coordinate here. */
const plain = (values: ArrayLike<number>): number[] => Array.from(values).map((value) => value + 0);

/** Every triangle must wind counter-clockwise seen from outside, i.e. its geometric normal agrees with the stored face normal. */
function expectOutwardWinding(geometry: Esp32VoxelGeometry): void {
  const { positions, normals } = geometry;
  for (let t = 0; t < geometry.triangleCount; t += 1) {
    const o = t * 9;
    const e1 = [positions[o + 3] - positions[o], positions[o + 4] - positions[o + 1], positions[o + 5] - positions[o + 2]];
    const e2 = [positions[o + 6] - positions[o], positions[o + 7] - positions[o + 1], positions[o + 8] - positions[o + 2]];
    const cross = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    const dot = cross[0] * normals[o] + cross[1] * normals[o + 1] + cross[2] * normals[o + 2];
    expect(dot, `triangle ${t} faces outward`).toBeGreaterThan(0);
  }
}

describe('ESP32 voxel scene geometry (issue #44)', () => {
  it('a single cell with every face exposed is 6 faces / 12 triangles spanning exactly one cell, all facing outward', () => {
    const geometry = buildEsp32VoxelGeometry(artifact({}), { x: 0, y: 0, z: 0 });
    expect(geometry.faceCount).toBe(6);
    expect(geometry.triangleCount).toBe(12);
    expect(geometry.positions).toHaveLength(12 * 9);
    expect(plain(geometry.bounds.min)).toEqual([0, 0, -0.25]);   /* project y in [0,0.25] becomes viewport -z */
    expect(plain(geometry.bounds.max)).toEqual([0.25, 0.25, 0]);
    expectOutwardWinding(geometry);
  });

  it('builds only the faces the mask marks as exposed, with the right outward normal in the Y-up viewport', () => {
    const top = buildEsp32VoxelGeometry(artifact({ faceMasks: new Uint8Array([BIT.POS_Z]) }), { x: 0, y: 0, z: 0 });
    expect(top.triangleCount).toBe(2);
    expect(plain(top.normals.slice(0, 3))).toEqual([0, 1, 0]);               /* project +Z (up) is viewport +Y */
    expect(Array.from(top.triangleFace)).toEqual([4, 4]);
    const southProject = buildEsp32VoxelGeometry(artifact({ faceMasks: new Uint8Array([BIT.NEG_Y]) }), { x: 0, y: 0, z: 0 });
    expect(plain(southProject.normals.slice(0, 3))).toEqual([0, 0, 1]);      /* project -Y is viewport +Z */
    const east = buildEsp32VoxelGeometry(artifact({ faceMasks: new Uint8Array([BIT.POS_X | BIT.NEG_X]) }), { x: 0, y: 0, z: 0 });
    expect(east.faceCount).toBe(2);
    expectOutwardWinding(top); expectOutwardWinding(southProject); expectOutwardWinding(east);
    expect(buildEsp32VoxelGeometry(artifact({ faceMasks: new Uint8Array([0]) }), { x: 0, y: 0, z: 0 }).triangleCount).toBe(0);
  });

  it('decodes the row-major index on a non-cubic grid with a negative origin, and places the cell by its minimum corner', () => {
    /* dims 3 x 2 x 4, cell (ix=2, iy=1, iz=3): index = ((3*2)+1)*3+2 = 23 */
    const value = artifact({ origin: [-1, -0.5, -0.25], dimensions: [3, 2, 4], voxelSizeMeters: 0.5, indices: new Uint32Array([23]) });
    const cell = decodeVoxelCell(value, 0);
    expect(cell.grid).toEqual([2, 1, 3]);
    expect(cell.minCorner).toEqual([0, 0, 1.25]);
    expect(cell.center).toEqual([0.25, 0.25, 1.5]);
    expect(() => decodeVoxelCell(value, 1)).toThrow(RangeError);
    expect(() => decodeVoxelCell(value, -1)).toThrow(RangeError);
  });

  it('keeps the transmitted order and maps every triangle back to its cell for picking', () => {
    const value = artifact({ dimensions: [4, 1, 1], indices: new Uint32Array([0, 2, 3]), faceMasks: new Uint8Array([BIT.POS_Z, BIT.POS_Z | BIT.NEG_Z, BIT.NEG_X]) });
    const geometry = buildEsp32VoxelGeometry(value, { x: 0, y: 0, z: 0 });
    expect(Array.from(geometry.triangleCell)).toEqual([0, 0, 1, 1, 1, 1, 2, 2]);
    expect(exposedFaceCount(value)).toBe(4);
  });

  it('subtracts the viewport centre like every other scan layer (project frame in, viewport frame out)', () => {
    const a = buildEsp32VoxelGeometry(artifact({}), { x: 0, y: 0, z: 0 });
    const b = buildEsp32VoxelGeometry(artifact({}), { x: 1, y: 2, z: 3 });
    expect(b.bounds.min[0]).toBeCloseTo(a.bounds.min[0] - 1, 6);
    expect(b.bounds.min[1]).toBeCloseTo(a.bounds.min[1] - 3, 6);    /* project z is viewport y */
    expect(b.bounds.min[2]).toBeCloseTo(a.bounds.min[2] + 2, 6);    /* project y is viewport -z */
    expect(plain(b.normals)).toEqual(plain(a.normals));  /* directions do not depend on the centre */
  });
});

const SESSION = resolve(__dirname, '../../../logs/phone-lab/2026-11-21_233928/saves');
describe.skipIf(!existsSync(SESSION))('ESP32 voxel scene from the real phone-lab preview (119 cells)', () => {
  const read = (kind: string): Uint8Array => new Uint8Array(readFileSync(resolve(SESSION, readdirSync(SESSION).find((entry) => entry.includes(`_${kind}_`))!)));

  it('builds exactly the transmitted cells and exposed faces, every one facing outward, inside the grid', () => {
    const preview = decodeVoxelPreviewArtifact(read('preview'), read('scan'));
    const geometry = buildEsp32VoxelGeometry(preview, { x: 0, y: 0, z: 0 });
    expect(geometry.cellCount).toBe(119);
    let expectedFaces = 0;
    for (const mask of preview.faceMasks) for (let bit = 0; bit < 6; bit += 1) if ((mask & (1 << bit)) !== 0) expectedFaces += 1;
    expect(geometry.faceCount).toBe(expectedFaces);
    expect(geometry.triangleCount).toBe(expectedFaces * 2);
    expect(new Set(geometry.triangleCell).size, 'every transmitted cell contributes at least one face').toBe(119);
    expectOutwardWinding(geometry);
    const [dx, dy, dz] = preview.dimensions;
    const size = preview.voxelSizeMeters;
    expect(geometry.bounds.max[0] - geometry.bounds.min[0]).toBeLessThanOrEqual(dx * size + 1e-6);
    expect(geometry.bounds.max[1] - geometry.bounds.min[1]).toBeLessThanOrEqual(dz * size + 1e-6);
    expect(geometry.bounds.max[2] - geometry.bounds.min[2]).toBeLessThanOrEqual(dy * size + 1e-6);
  });
});
