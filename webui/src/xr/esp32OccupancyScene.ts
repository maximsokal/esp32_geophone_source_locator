/**
 * The solver occupancy (GOCCUPY1) as 3D geometry (issue #44): what the ESP32 localisation actually receives, which is NOT the TFT preview and NOT
 * the phone's own display counters.
 *
 *  - OCCUPIED and FREE are the transmitted cell lists, drawn at their exact int16 grid cells (cell i spans [i*size, (i+1)*size], project frame, Z up);
 *  - UNKNOWN is never transmitted: it is the implicit remainder of the candidate envelope, so it is reported as a label and NEVER drawn as cells
 *    (drawing it would turn "not observed" into something that looks measured, and the volume can be huge);
 *  - only the faces a cell does not share with a cell of the same class are built, as one batched geometry per class, capped so a 1 MiB artifact
 *    cannot stall a phone (the cap is reported, never silent).
 */

import type { DecodedOccupancyArtifact } from './occupancySolverArtifact';
import { buildCellFaceGeometry, type Esp32VoxelGeometry, type VoxelSceneCentre } from './esp32VoxelScene';

export const MAX_SOLVER_VOXEL_FACES = 120_000;

export type SolverVoxelClass = 'occupied' | 'free';

export interface SolverVoxelCell {
  readonly cls: SolverVoxelClass;
  readonly grid: readonly [number, number, number];
  readonly minCorner: readonly [number, number, number];
  readonly center: readonly [number, number, number];
}

export interface SolverVoxelGeometry {
  occupied: Esp32VoxelGeometry;
  free: Esp32VoxelGeometry;
  voxelSizeMeters: number;
  occupiedCount: number;
  freeCount: number;
  /** Cells whose faces were left out because the face cap was reached (0 = everything is drawn). */
  omittedCells: number;
  /** Every cell of each class in the transmitted order, for picking. */
  occupiedCells: Int16Array;
  freeCells: Int16Array;
}

function key(x: number, y: number, z: number): number {
  return ((x + 32768) * 65536 + (y + 32768)) * 65536 + (z + 32768);
}

/** Exposed-face mask per cell of one class, against the other cells of the SAME class. Bit order as VOXEL_FACE_BITS. */
export function exposedFaceMasks(cells: Int16Array): Uint8Array {
  const count = cells.length / 3;
  const present = new Set<number>();
  for (let i = 0; i < count; i += 1) present.add(key(cells[i * 3], cells[i * 3 + 1], cells[i * 3 + 2]));
  const masks = new Uint8Array(count);
  for (let i = 0; i < count; i += 1) {
    const x = cells[i * 3]; const y = cells[i * 3 + 1]; const z = cells[i * 3 + 2];
    let mask = 0;
    if (!present.has(key(x + 1, y, z))) mask |= 1;
    if (!present.has(key(x - 1, y, z))) mask |= 2;
    if (!present.has(key(x, y + 1, z))) mask |= 4;
    if (!present.has(key(x, y - 1, z))) mask |= 8;
    if (!present.has(key(x, y, z + 1))) mask |= 16;
    if (!present.has(key(x, y, z - 1))) mask |= 32;
    masks[i] = mask;
  }
  return masks;
}

function facesOf(masks: Uint8Array): number {
  let faces = 0;
  for (const mask of masks) for (let bit = 0; bit < 6; bit += 1) if ((mask & (1 << bit)) !== 0) faces += 1;
  return faces;
}

function classGeometry(cells: Int16Array, size: number, centre: VoxelSceneCentre, faceBudget: number): { geometry: Esp32VoxelGeometry; omitted: number } {
  const masks = exposedFaceMasks(cells);
  let used = 0;
  let kept = masks.length;
  for (let i = 0; i < masks.length; i += 1) {
    const faces = facesOf(masks.subarray(i, i + 1));
    if (used + faces > faceBudget) { kept = i; break; }
    used += faces;
  }
  const minCorners = new Float64Array(kept * 3);
  for (let i = 0; i < kept * 3; i += 1) minCorners[i] = cells[i] * size;
  return { geometry: buildCellFaceGeometry(minCorners, masks.subarray(0, kept), size, centre), omitted: masks.length - kept };
}

export function buildSolverVoxelGeometry(decoded: DecodedOccupancyArtifact, centre: VoxelSceneCentre, maxFaces = MAX_SOLVER_VOXEL_FACES): SolverVoxelGeometry {
  const size = decoded.header.voxel_size_m;
  /* OCCUPIED first: it is what the solver constrains with, so it gets the budget before the FREE context does. */
  const occupied = classGeometry(decoded.occupied, size, centre, maxFaces);
  const free = classGeometry(decoded.free, size, centre, Math.max(0, maxFaces - occupied.geometry.faceCount));
  return {
    occupied: occupied.geometry,
    free: free.geometry,
    voxelSizeMeters: size,
    occupiedCount: decoded.occupied.length / 3,
    freeCount: decoded.free.length / 3,
    omittedCells: occupied.omitted + free.omitted,
    occupiedCells: decoded.occupied,
    freeCells: decoded.free,
  };
}

export function solverVoxelCell(geometry: SolverVoxelGeometry, cls: SolverVoxelClass, ordinal: number): SolverVoxelCell {
  const cells = cls === 'occupied' ? geometry.occupiedCells : geometry.freeCells;
  if (!Number.isInteger(ordinal) || ordinal < 0 || ordinal * 3 + 2 >= cells.length) throw new RangeError(`Solver voxel ordinal ${ordinal} is outside the artifact.`);
  const size = geometry.voxelSizeMeters;
  const grid: [number, number, number] = [cells[ordinal * 3], cells[ordinal * 3 + 1], cells[ordinal * 3 + 2]];
  const minCorner: [number, number, number] = [grid[0] * size, grid[1] * size, grid[2] * size];
  return { cls, grid, minCorner, center: [minCorner[0] + size / 2, minCorner[1] + size / 2, minCorner[2] + size / 2] };
}
