import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { buildSolverVoxelGeometry, exposedFaceMasks, solverVoxelCell } from '../../src/xr/esp32OccupancyScene';
import { decodeOccupancySolverArtifact, type DecodedOccupancyArtifact } from '../../src/xr/occupancySolverArtifact';

const decoded = (occupied: number[], free: number[], size = 0.25): DecodedOccupancyArtifact => ({
  header: {
    schema: 2, frame_id: 'scan-crc32:00000001', voxel_size_m: size, below_surface_m: 0, envelope: null,
    free_count: free.length / 3, occupied_count: occupied.length / 3, saturated: false, created_at_ms: 0, registration: null,
  },
  free: Int16Array.from(free),
  occupied: Int16Array.from(occupied),
  legacyRegistrationIgnored: false,
});
const centre = { x: 0, y: 0, z: 0 };

describe('solver occupancy geometry GOCCUPY1 (issue #44)', () => {
  it('does not treat a neighbour beyond the int16 grid as a cell: cells (0,32767,0) and (1,-32768,0) are not adjacent (audit 2026-11-22)', () => {
    const masks = Array.from(exposedFaceMasks(Int16Array.from([0, 32767, 0, 1, -32768, 0])));
    expect(masks).toEqual([63, 63]);
    const edge = Array.from(exposedFaceMasks(Int16Array.from([-32768, -32768, -32768, 32767, 32767, 32767])));
    expect(edge).toEqual([63, 63]);
  });

  it('a lone cell exposes all 6 faces; two face-sharing cells hide the shared faces and keep 10', () => {
    expect(Array.from(exposedFaceMasks(Int16Array.from([0, 0, 0])))).toEqual([63]);
    const pair = exposedFaceMasks(Int16Array.from([0, 0, 0, 1, 0, 0]));
    expect(Array.from(pair)).toEqual([63 & ~1, 63 & ~2]);     /* +X of the first and -X of the second are shared */
    const geometry = buildSolverVoxelGeometry(decoded([0, 0, 0, 1, 0, 0], []), centre);
    expect(geometry.occupied.faceCount).toBe(10);
    expect(geometry.occupied.cellCount).toBe(2);
    expect(geometry.free.faceCount).toBe(0);
  });

  it('places cells by their absolute int16 grid index, negative indices included (cell i spans [i*size, (i+1)*size])', () => {
    const geometry = buildSolverVoxelGeometry(decoded([-2, 3, 4], []), centre);
    const cell = solverVoxelCell(geometry, 'occupied', 0);
    expect(cell.grid).toEqual([-2, 3, 4]);
    expect(cell.minCorner).toEqual([-0.5, 0.75, 1]);
    expect(cell.center).toEqual([-0.375, 0.875, 1.125]);
    expect(() => solverVoxelCell(geometry, 'free', 0)).toThrow(RangeError);
  });

  it('keeps OCCUPIED and FREE apart: each class hides only faces shared with its own class', () => {
    const geometry = buildSolverVoxelGeometry(decoded([0, 0, 0], [1, 0, 0]), centre);
    expect(geometry.occupied.faceCount).toBe(6);
    expect(geometry.free.faceCount).toBe(6);                  /* an OCCUPIED neighbour does not hide a FREE face */
    expect(geometry.occupiedCount).toBe(1);
    expect(geometry.freeCount).toBe(1);
  });

  it('never invents UNKNOWN cells: only the transmitted lists are drawn', () => {
    const geometry = buildSolverVoxelGeometry(decoded([0, 0, 0, 0, 0, 1], [5, 5, 5]), centre);
    expect(geometry.occupied.cellCount + geometry.free.cellCount).toBe(3);
  });

  it('caps the faces, gives OCCUPIED the budget first, and reports what it left out', () => {
    const occupied: number[] = [];
    for (let i = 0; i < 10; i += 1) occupied.push(i * 2, 0, 0);       /* 10 isolated cells = 60 faces */
    const free = [100, 0, 0, 102, 0, 0];
    const capped = buildSolverVoxelGeometry(decoded(occupied, free), centre, 40);
    expect(capped.occupied.faceCount).toBeLessThanOrEqual(40);
    expect(capped.occupied.cellCount).toBe(6);                        /* 6 cells x 6 faces = 36 <= 40 < 42 */
    expect(capped.free.cellCount).toBe(0);
    expect(capped.omittedCells).toBe(4 + 2);
    expect(buildSolverVoxelGeometry(decoded(occupied, free), centre).omittedCells).toBe(0);
  });

  it('is deterministic for the same cells', () => {
    const a = buildSolverVoxelGeometry(decoded([0, 0, 0, 1, 0, 0, 0, 1, 0], [3, 3, 3]), centre);
    const b = buildSolverVoxelGeometry(decoded([0, 0, 0, 1, 0, 0, 0, 1, 0], [3, 3, 3]), centre);
    expect(Array.from(a.occupied.positions)).toEqual(Array.from(b.occupied.positions));
    expect(Array.from(a.free.triangleCell)).toEqual(Array.from(b.free.triangleCell));
  });
});

const SESSION = resolve(__dirname, '../../../logs/phone-lab/2026-11-21_233928/saves');
describe.skipIf(!existsSync(SESSION))('solver geometry from the real phone-lab occupancy (28 FREE / 127 OCCUPIED)', () => {
  it('draws exactly the transmitted cells with nothing omitted', () => {
    const name = readdirSync(SESSION).find((entry) => entry.includes('_occupancy_'))!;
    const occupancy = decodeOccupancySolverArtifact(new Uint8Array(readFileSync(resolve(SESSION, name))));
    const geometry = buildSolverVoxelGeometry(occupancy, centre);
    expect(geometry.occupiedCount).toBe(127);
    expect(geometry.freeCount).toBe(28);
    expect(geometry.omittedCells).toBe(0);
    expect(geometry.occupied.cellCount).toBe(127);
    expect(geometry.free.cellCount).toBe(28);
    expect(geometry.occupied.triangleCount).toBe(geometry.occupied.faceCount * 2);
    expect(geometry.voxelSizeMeters).toBe(0.25);
  });
});
