import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it } from 'vitest';

import { decodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
import { encodeVoidScene, GRID, toScan, TRENCH, VOID_SCENE_REGISTRATION, VOXEL } from './support/voidScene';

/*
 * End-to-end vector for the empty/filled space the phone scan hands to the
 * ESP32 for source location (TDCAS, PSAFUVS, SUEA). The scene, in the
 * sensor (geophone) frame, 0.1 m voxels over a 3 m x 3 m rubble surface:
 *
 *   - OCCUPIED: the observed surface skin, z in [-0.1, 0), except the trench;
 *   - FREE:     open air z in [0, 0.2) everywhere, and a trench (a gap in the
 *               rubble) x in [1.4, 1.7), all y, from z = -0.9 up to the air;
 *   - UNKNOWN:  everything else inside the candidate volume (the unseen
 *               interior) -- implicit, never stored.
 *
 * The scan frame is NOT the sensor frame: registration rotates 90 degrees
 * about Z and translates (10, -5, 2), so the ESP32 must apply it to use the
 * sectors at all. components/localization_calculate/tests/
 * localization_calculate_scan_methods_test.c parses these exact bytes and
 * locates a source behind the trench with every method. Regenerate with
 * UPDATE_VECTORS=1 only after an intentional format change.
 */
const VECTOR_PATH = resolve(__dirname, '../../../tests/vectors/scan_occupancy_void_scene_v1.bin');

describe('void-scene occupancy vector for ESP32 source location', () => {
  it('stores every empty (FREE) and filled (OCCUPIED) sector exactly once', () => {
    const decoded = decodeOccupancySolverArtifact(encodeVoidScene());
    const airCells = GRID * GRID * 2;
    const trenchCells = (TRENCH.highIndex - TRENCH.lowIndex + 1) * GRID * -TRENCH.bottomIndex;
    const surfaceCells = GRID * GRID - (TRENCH.highIndex - TRENCH.lowIndex + 1) * GRID;
    expect(decoded.header.free_count).toBe(airCells + trenchCells);
    expect(decoded.header.occupied_count).toBe(surfaceCells);

    const keys = (values: Int16Array) => {
      const set = new Set<string>();
      for (let index = 0; index < values.length; index += 3) set.add(`${values[index]}:${values[index + 1]}:${values[index + 2]}`);
      return set;
    };
    const free = keys(decoded.free);
    const occupied = keys(decoded.occupied);
    expect(free.size).toBe(decoded.header.free_count);
    expect(occupied.size).toBe(decoded.header.occupied_count);
    // No sector is both empty and filled.
    for (const key of occupied) expect(free.has(key)).toBe(false);
  });

  it('indexes sectors in the SCAN frame through the registration', () => {
    const decoded = decodeOccupancySolverArtifact(encodeVoidScene());
    const cell = (x: number, y: number, z: number) => toScan(x, y, z).map((value) => Math.floor(value / VOXEL)).join(':');
    const has = (values: Int16Array, key: string) => {
      for (let index = 0; index < values.length; index += 3) {
        if (`${values[index]}:${values[index + 1]}:${values[index + 2]}` === key) return true;
      }
      return false;
    };
    expect(has(decoded.free, cell(1.55, 1.55, -0.55))).toBe(true); // inside the trench
    expect(has(decoded.free, cell(0.55, 0.55, 0.15))).toBe(true); // open air
    expect(has(decoded.occupied, cell(0.55, 0.55, -0.05))).toBe(true); // surface skin
    expect(has(decoded.free, cell(0.55, 0.55, -0.55))).toBe(false); // unseen interior: never FREE
    expect(has(decoded.occupied, cell(0.55, 0.55, -0.55))).toBe(false);
    expect(decoded.header.registration).toEqual(VOID_SCENE_REGISTRATION);
    expect(decoded.header.below_surface_m).toBe(2);
  });

  it('matches the vector the ESP32 host test consumes', () => {
    const bytes = encodeVoidScene();
    if (process.env.UPDATE_VECTORS === '1' || !existsSync(VECTOR_PATH)) {
      writeFileSync(VECTOR_PATH, bytes);
    }
    expect(Array.from(new Uint8Array(readFileSync(VECTOR_PATH)))).toEqual(Array.from(bytes));
  });
});
