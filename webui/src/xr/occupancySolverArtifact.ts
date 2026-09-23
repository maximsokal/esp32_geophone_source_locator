/**
 * MAX-162: compact, loss-free FREE/OCCUPIED occupancy for ESP32-side source
 * localization -- a bounded derivative of the saved scan, never the raw
 * depth stream.
 *
 * Binary layout (little endian), mirrored by components/scan_occupancy:
 *
 *   0  "GOCCUPY1"                 magic/version
 *   8  u32 header_length          UTF-8 JSON header bytes
 *   12 u32 payload_length         = 6 * (free_count + occupied_count)
 *   16 u32 crc32                  over every byte from offset 20 to the end
 *   20 header JSON
 *   .. free_count     x (i16 ix, i16 iy, i16 iz)   FREE voxel indices
 *   .. occupied_count x (i16 ix, i16 iy, i16 iz)   OCCUPIED voxel indices
 *
 * A voxel index is floor(coordinate / voxel_size_m) in the scan's project
 * frame (meters, Z up). UNKNOWN is implicit: every cell of the candidate
 * volume (the envelope extended below_surface_m downward) that is neither
 * FREE nor OCCUPIED. UNKNOWN is never stored as FREE.
 *
 * `registration` is the sensor(localization)->scan rigid transform. It is
 * null until an AprilTag/ArUco registration (MAX-145) exists; the ESP32 then
 * fails closed and never applies the occupancy constraint.
 */

import type { ScanPointCloudSnapshot } from './depthPointCloud';

const MAGIC = new TextEncoder().encode('GOCCUPY1');
const FIXED_HEADER_BYTES = 20;
export const OCCUPANCY_ARTIFACT_SCHEMA = 1;
export const MAX_OCCUPANCY_ARTIFACT_BYTES = 1024 * 1024;
const INT16_MIN = -32768;
const INT16_MAX = 32767;

export interface OccupancyRegistration {
  /** How the transform was obtained, e.g. 'apriltag'. */
  source: string;
  /** The frozen session geometry version this registration was made against. */
  geometry_computed_at_ms: number;
  /** Row-major 4x4 homogeneous sensor->scan transform (meters). */
  sensor_to_scan: number[];
  /** RMS registration residual (meters). */
  residual_m: number;
}

export interface OccupancyArtifactHeader {
  schema: number;
  frame_id: string;
  voxel_size_m: number;
  below_surface_m: number;
  envelope: { min: [number, number, number]; max: [number, number, number] } | null;
  free_count: number;
  occupied_count: number;
  saturated: boolean;
  created_at_ms: number;
  registration: OccupancyRegistration | null;
}

export interface DecodedOccupancyArtifact {
  header: OccupancyArtifactHeader;
  free: Int16Array;
  occupied: Int16Array;
}

export function occupancyCrc32(bytes: Uint8Array, start = 0): number {
  let crc = 0xffffffff;
  for (let index = start; index < bytes.length; index += 1) {
    crc ^= bytes[index];
    for (let bit = 0; bit < 8; bit += 1) {
      const mask = -(crc & 1);
      crc = (crc >>> 1) ^ (0xedb88320 & mask);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function voxelIndices(positions: Float32Array, voxelSize: number, label: string): Int16Array {
  const result = new Int16Array(positions.length);
  for (let index = 0; index < positions.length; index += 1) {
    const cell = Math.floor(positions[index] / voxelSize);
    if (!Number.isFinite(cell) || cell < INT16_MIN || cell > INT16_MAX) {
      throw new Error(`${label} voxel index out of the int16 range.`);
    }
    result[index] = cell;
  }
  return result;
}

export interface OccupancyArtifactOptions {
  /** Identity of the exact saved scan this occupancy was derived from. */
  frameId: string;
  createdAtMs: number;
  registration?: OccupancyRegistration | null;
}

/**
 * FREE/OCCUPIED voxel centers are indexed at the FINE occupancy voxel size
 * (`occupancyFineVoxelSizeMeters`); `occupancyVoxelSizeMeters` may be a
 * coarser UNKNOWN display grid (MAX-213). Snapshots saved before the fine
 * size was recorded fall back to the display size, which equals it
 * whenever no UNKNOWN stride was applied.
 */
export function encodeOccupancySolverArtifact(
  snapshot: Pick<ScanPointCloudSnapshot,
    'freeAirPositions' | 'occupiedSurfacePositions' | 'occupancyEnvelopeBounds' |
    'occupancySolverCandidateBelowSurfaceMeters' | 'occupancyCellsSaturated' |
    'occupancyFineVoxelSizeMeters' | 'occupancyVoxelSizeMeters'>,
  options: OccupancyArtifactOptions,
): Uint8Array {
  const fineVoxelSizeMeters = snapshot.occupancyFineVoxelSizeMeters ?? snapshot.occupancyVoxelSizeMeters;
  if (!(fineVoxelSizeMeters > 0)) throw new Error('Invalid occupancy voxel size.');
  if (options.frameId.trim().length === 0) throw new Error('Occupancy artifact needs a frame id.');
  const registration = options.registration ?? null;
  if (registration !== null && registration.sensor_to_scan.length !== 16) {
    throw new Error('Occupancy registration must be a 4x4 matrix.');
  }

  const free = voxelIndices(snapshot.freeAirPositions, fineVoxelSizeMeters, 'FREE');
  const occupied = voxelIndices(snapshot.occupiedSurfacePositions, fineVoxelSizeMeters, 'OCCUPIED');
  const bounds = snapshot.occupancyEnvelopeBounds;
  const header: OccupancyArtifactHeader = {
    schema: OCCUPANCY_ARTIFACT_SCHEMA,
    frame_id: options.frameId,
    voxel_size_m: fineVoxelSizeMeters,
    below_surface_m: snapshot.occupancySolverCandidateBelowSurfaceMeters ?? 0,
    envelope: bounds === null
      ? null
      : { min: [bounds.min[0], bounds.min[1], bounds.min[2]], max: [bounds.max[0], bounds.max[1], bounds.max[2]] },
    free_count: free.length / 3,
    occupied_count: occupied.length / 3,
    saturated: snapshot.occupancyCellsSaturated === true,
    created_at_ms: options.createdAtMs,
    registration,
  };

  const headerBytes = new TextEncoder().encode(JSON.stringify(header));
  const payloadBytes = (free.length + occupied.length) * Int16Array.BYTES_PER_ELEMENT;
  const total = FIXED_HEADER_BYTES + headerBytes.length + payloadBytes;
  if (total > MAX_OCCUPANCY_ARTIFACT_BYTES) throw new Error(`Occupancy artifact is too large (${total} bytes).`);

  const result = new Uint8Array(total);
  result.set(MAGIC, 0);
  const view = new DataView(result.buffer);
  view.setUint32(8, headerBytes.length, true);
  view.setUint32(12, payloadBytes, true);
  result.set(headerBytes, FIXED_HEADER_BYTES);
  let offset = FIXED_HEADER_BYTES + headerBytes.length;
  for (const values of [free, occupied]) {
    for (let index = 0; index < values.length; index += 1) {
      view.setInt16(offset, values[index], true);
      offset += 2;
    }
  }
  view.setUint32(16, occupancyCrc32(result, FIXED_HEADER_BYTES), true);
  return result;
}

export function decodeOccupancySolverArtifact(source: Uint8Array): DecodedOccupancyArtifact {
  if (source.byteLength < FIXED_HEADER_BYTES || source.byteLength > MAX_OCCUPANCY_ARTIFACT_BYTES) {
    throw new Error('Invalid occupancy artifact size.');
  }
  for (let index = 0; index < MAGIC.length; index += 1) {
    if (source[index] !== MAGIC[index]) throw new Error('Occupancy artifact magic/version mismatch.');
  }
  const view = new DataView(source.buffer, source.byteOffset, source.byteLength);
  const headerLength = view.getUint32(8, true);
  const payloadLength = view.getUint32(12, true);
  if (headerLength === 0 || FIXED_HEADER_BYTES + headerLength + payloadLength !== source.byteLength) {
    throw new Error('Occupancy artifact length table is inconsistent.');
  }
  if (occupancyCrc32(source, FIXED_HEADER_BYTES) !== view.getUint32(16, true)) {
    throw new Error('Occupancy artifact checksum mismatch.');
  }
  const header = JSON.parse(
    new TextDecoder().decode(source.subarray(FIXED_HEADER_BYTES, FIXED_HEADER_BYTES + headerLength)),
  ) as OccupancyArtifactHeader;
  if (header.schema !== OCCUPANCY_ARTIFACT_SCHEMA) throw new Error('Unsupported occupancy artifact schema.');
  if (payloadLength !== 6 * (header.free_count + header.occupied_count)) {
    throw new Error('Occupancy artifact counts disagree with the payload.');
  }
  let offset = FIXED_HEADER_BYTES + headerLength;
  const read = (count: number): Int16Array => {
    const values = new Int16Array(count * 3);
    for (let index = 0; index < values.length; index += 1) {
      values[index] = view.getInt16(offset, true);
      offset += 2;
    }
    return values;
  };
  const free = read(header.free_count);
  const occupied = read(header.occupied_count);
  return { header, free, occupied };
}
