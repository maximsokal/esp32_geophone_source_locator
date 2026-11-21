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
 * Schema 2 registrations map sensor Z-up coordinates into the project
 * Z-up frame. Schema 1 registered artifacts remain readable, but their
 * ambiguous-frame registration is ignored by both readers.
 */

import type { ScanPointCloudSnapshot } from './depthPointCloud';
import { localFloorMatrixToProject } from './projectFrame';
import { registrationTransformIssue } from './registrationApplicability';
import { crc32 } from './crc32';
import { createScanIdentity, type ScanIdentity } from './scanIdentity';

const MAGIC = new TextEncoder().encode('GOCCUPY1');
const FIXED_HEADER_BYTES = 20;
export const OCCUPANCY_ARTIFACT_SCHEMA = 2;
export const LEGACY_OCCUPANCY_ARTIFACT_SCHEMA = 1;
export const MAX_OCCUPANCY_ARTIFACT_BYTES = 1024 * 1024;
/* Sanity bound only: production clamps fine voxels to 0.08..0.50 m, fixtures may use coarser grids. */
const MAX_OCCUPANCY_VOXEL_SIZE_METERS = 10;
/** Same lower bound as the ESP32 parser: a finer cell is not physical and would make the ray sampler unbounded. */
const MIN_OCCUPANCY_VOXEL_SIZE_METERS = 0.001;

/** An envelope must be finite and ordered (min <= max per axis); an inverted box would veto every source candidate. */
function assertOrderedEnvelope(min: readonly number[], max: readonly number[]): void {
  for (let axis = 0; axis < 3; axis += 1) {
    if (!Number.isFinite(min[axis]) || !Number.isFinite(max[axis]) || min[axis] > max[axis]) {
      throw new Error('Invalid occupancy envelope: bounds must be finite with min <= max on every axis.');
    }
  }
}
const INT16_MIN = -32768;
const INT16_MAX = 32767;
/*
 * The smallest voxel the ESP32 source calculation receives. The search targets a source within about 2 cubic metres
 * (a cube of ~1.26 m): 0.25 m cells still split that into five per axis, while the phone's finer evidence (0.18 m by
 * default) would only multiply the cells the device parses and looks up per candidate. The phone keeps its fine grid
 * for display; only the solver artifact is coarsened (see coarsenOccupancyForSolver).
 */
export const OCCUPANCY_SOLVER_MIN_VOXEL_SIZE_METERS = 0.25;

export interface OccupancyRegistration {
  /** Omitted only by legacy in-memory callers; those matrices are local-floor. */
  frame?: 'webxr_local_floor' | 'project_z_up';
  /** How the transform was obtained, e.g. 'apriltag'. */
  source: string;
  /** The frozen session geometry version this registration was made against. */
  geometry_computed_at_ms: number;
  /** Row-major sensor Z-up -> project Z-up matrix once encoded as schema 2. */
  sensor_to_scan: number[];
  /** RMS registration residual (meters). */
  residual_m: number;
  /**
   * Acceptance threshold (meters) the residual was judged against. REQUIRED (finite, > 0, and
   * `residual_m <= threshold_m + 1e-9`) for every registration written to or read from a schema 2
   * artifact; it is optional in the type only because the preview builders reuse this shape for
   * in-memory transform checks that never reach the occupancy encoder.
   */
  threshold_m?: number;
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
  /** SHA-256 of the scan this occupancy was derived from (lowercase hex); absent in artifacts written before issue #34. */
  scan_sha256?: string;
}

export interface DecodedOccupancyArtifact {
  header: OccupancyArtifactHeader;
  free: Int16Array;
  occupied: Int16Array;
  legacyRegistrationIgnored: boolean;
}

const REGISTRATION_QUALITY_EPSILON_M = 1e-9;
let occupancyDecodeInvocations = 0;
let occupancyCrcInvocations = 0;
let occupancyCrcBytesVisited = 0;

export function getOccupancyArtifactMetrics(): Readonly<{
  decodeInvocations: number;
  crcInvocations: number;
  crcBytesVisited: number;
}> {
  return { decodeInvocations: occupancyDecodeInvocations, crcInvocations: occupancyCrcInvocations, crcBytesVisited: occupancyCrcBytesVisited };
}

export function resetOccupancyArtifactMetrics(): void {
  occupancyDecodeInvocations = 0;
  occupancyCrcInvocations = 0;
  occupancyCrcBytesVisited = 0;
}

function assertRegistrationQuality(value: OccupancyRegistration): void {
  const threshold = value.threshold_m;
  if (typeof threshold !== 'number' || !Number.isFinite(threshold) || !(threshold > 0)) {
    throw new Error('Occupancy registration needs a finite positive threshold_m.');
  }
  if (typeof value.residual_m !== 'number' || !Number.isFinite(value.residual_m) || value.residual_m < 0 ||
      value.residual_m > threshold + REGISTRATION_QUALITY_EPSILON_M) {
    throw new Error('Occupancy registration residual exceeds its threshold.');
  }
}

function registrationInProjectFrame(value: OccupancyRegistration | null): OccupancyRegistration | null {
  if (value === null) return null;
  const frame: unknown = value.frame;
  if (frame !== undefined && frame !== 'webxr_local_floor' && frame !== 'project_z_up') {
    throw new Error('Unknown occupancy registration frame; expected project_z_up.');
  }
  if (value.sensor_to_scan.length !== 16 || value.sensor_to_scan.some((entry) => !Number.isFinite(entry))) {
    throw new Error('Occupancy registration must be a finite 4x4 matrix.');
  }
  assertRegistrationQuality(value);
  const projectMatrix = frame === 'project_z_up'
    ? [...value.sensor_to_scan]
    : localFloorMatrixToProject(value.sensor_to_scan);
  const transformIssue = registrationTransformIssue(projectMatrix);
  if (transformIssue === 'reflected_transform') throw new Error('reflected_transform');
  if (transformIssue !== null) throw new Error('Occupancy registration transform must be a proper rigid transform.');
  return {
    ...value,
    frame: 'project_z_up',
    sensor_to_scan: projectMatrix,
  };
}

export function occupancyCrc32(bytes: Uint8Array, start = 0): number {
  occupancyCrcInvocations += 1;
  occupancyCrcBytesVisited += bytes.length - start;
  return crc32(bytes, start);
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

function assertUniqueVoxelKeys(free: Int16Array, occupied: Int16Array): void {
  const seen = new Set<number>();
  for (const values of [free, occupied]) {
    for (let index = 0; index < values.length; index += 3) {
      const key = (((values[index] + 32768) * 65536 + (values[index + 1] + 32768)) * 65536) +
        values[index + 2] + 32768;
      if (seen.has(key)) throw new Error('Duplicate occupancy voxel key.');
      seen.add(key);
    }
  }
}

export interface OccupancyArtifactOptions {
  /** Identity of the exact saved scan this occupancy was derived from. */
  frameId: string;
  createdAtMs: number;
  registration?: OccupancyRegistration | null;
  /** SHA-256 of the exact saved scan (ScanIdentity.sha256): the ESP32 then refuses this occupancy for another scan with the same CRC32. */
  scanSha256?: string;
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
  if (!(fineVoxelSizeMeters >= MIN_OCCUPANCY_VOXEL_SIZE_METERS)) throw new Error('Invalid occupancy voxel size.');
  if (options.frameId.trim().length === 0) throw new Error('Occupancy artifact needs a frame id.');
  const registration = registrationInProjectFrame(options.registration ?? null);

  const free = voxelIndices(snapshot.freeAirPositions, fineVoxelSizeMeters, 'FREE');
  const occupied = voxelIndices(snapshot.occupiedSurfacePositions, fineVoxelSizeMeters, 'OCCUPIED');
  const bounds = snapshot.occupancyEnvelopeBounds;
  if (bounds !== null && bounds !== undefined) assertOrderedEnvelope(bounds.min, bounds.max);
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
    ...(options.scanSha256 === undefined ? {} : { scan_sha256: options.scanSha256 }),
  };
  if (header.scan_sha256 !== undefined && !/^[0-9a-f]{64}$/.test(header.scan_sha256)) throw new Error('Invalid scan SHA-256 for the occupancy artifact.');

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
  occupancyDecodeInvocations += 1;
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
  // Keep the decoder's independent checksum pass: callers decode a freshly encoded artifact to validate its emitted bytes.
  if (occupancyCrc32(source, FIXED_HEADER_BYTES) !== view.getUint32(16, true)) {
    throw new Error('Occupancy artifact checksum mismatch.');
  }
  const header = JSON.parse(
    new TextDecoder().decode(source.subarray(FIXED_HEADER_BYTES, FIXED_HEADER_BYTES + headerLength)),
  ) as OccupancyArtifactHeader;
  if (header.schema !== LEGACY_OCCUPANCY_ARTIFACT_SCHEMA && header.schema !== OCCUPANCY_ARTIFACT_SCHEMA) {
    throw new Error('Unsupported occupancy artifact schema.');
  }
  if (!Number.isFinite(header.voxel_size_m) || !(header.voxel_size_m >= MIN_OCCUPANCY_VOXEL_SIZE_METERS) ||
      header.voxel_size_m > MAX_OCCUPANCY_VOXEL_SIZE_METERS) {
    throw new Error('Invalid occupancy voxel size.');
  }
  if (header.scan_sha256 !== undefined && header.scan_sha256 !== null &&
      (typeof header.scan_sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(header.scan_sha256))) {
    throw new Error('Invalid occupancy scan_sha256.');
  }
  if (header.envelope !== null && header.envelope !== undefined) {
    const { min, max } = header.envelope as { min?: unknown; max?: unknown };
    if (!Array.isArray(min) || !Array.isArray(max) || min.length !== 3 || max.length !== 3) {
      throw new Error('Invalid occupancy envelope: expected 3-element min and max.');
    }
    assertOrderedEnvelope(min as number[], max as number[]);
  }
  const registrationPresent = header.registration !== null && header.registration !== undefined;
  const legacyRegistrationIgnored = header.schema === LEGACY_OCCUPANCY_ARTIFACT_SCHEMA && registrationPresent;
  const declaredRegistration = header.registration;
  if (header.schema === OCCUPANCY_ARTIFACT_SCHEMA && declaredRegistration !== null && declaredRegistration !== undefined &&
      (typeof declaredRegistration !== 'object' || declaredRegistration.frame !== 'project_z_up')) {
    throw new Error('Unknown occupancy registration frame; expected project_z_up.');
  }
  if (header.schema === OCCUPANCY_ARTIFACT_SCHEMA && declaredRegistration !== null && declaredRegistration !== undefined) {
    assertRegistrationQuality(declaredRegistration);
    const transformIssue = registrationTransformIssue(declaredRegistration.sensor_to_scan);
    if (transformIssue === 'reflected_transform') throw new Error('reflected_transform');
    if (transformIssue !== null) throw new Error('Occupancy registration transform must be a proper rigid transform.');
  }
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
  assertUniqueVoxelKeys(free, occupied);
  return {
    header: legacyRegistrationIgnored ? { ...header, registration: null } : header,
    free,
    occupied,
    legacyRegistrationIgnored,
  };
}

/** Identity of the exact saved scan an occupancy artifact was derived from. */
export function occupancyFrameIdForScan(scanArtifact: Uint8Array, identity?: ScanIdentity): string {
  return identity?.frameId ?? createScanIdentity(scanArtifact).frameId;
}

type OccupancyGridSnapshot = Pick<ScanPointCloudSnapshot,
  'freeAirPositions' | 'occupiedSurfacePositions' | 'occupancyEnvelopeBounds' |
  'occupancySolverCandidateBelowSurfaceMeters' | 'occupancyCellsSaturated' |
  'occupancyFineVoxelSizeMeters' | 'occupancyVoxelSizeMeters'>;

function cellKey(ix: number, iy: number, iz: number): string {
  return `${ix},${iy},${iz}`;
}

function cellSet(positions: Float32Array, size: number): Set<string> {
  const result = new Set<string>();
  for (let index = 0; index + 2 < positions.length; index += 3) {
    result.add(cellKey(Math.floor(positions[index] / size), Math.floor(positions[index + 1] / size),
      Math.floor(positions[index + 2] / size)));
  }
  return result;
}

/*
 * Re-grids the FREE / OCCUPIED evidence at `targetSize` without ever inventing empty space: FREE excludes source
 * candidates on the ESP32, so a coarse cell is FREE only when every fine cell overlapping it was observed FREE (and none
 * of them is OCCUPIED). A coarse cell holding any OCCUPIED fine centre is OCCUPIED; every other cell stays UNKNOWN,
 * which the device never treats as empty. Fine and coarse grids need not nest (0.18 m into 0.25 m).
 */
export function coarsenOccupancyForSolver<T extends OccupancyGridSnapshot>(snapshot: T, targetSize = OCCUPANCY_SOLVER_MIN_VOXEL_SIZE_METERS): T {
  const fine = snapshot.occupancyFineVoxelSizeMeters ?? snapshot.occupancyVoxelSizeMeters;
  if (!(fine > 0) || !(targetSize > 0) || fine >= targetSize) return snapshot;
  const fineFree = cellSet(snapshot.freeAirPositions, fine);
  const coarseOccupied = cellSet(snapshot.occupiedSurfacePositions, targetSize);
  const candidates = cellSet(snapshot.freeAirPositions, targetSize);
  const epsilon = fine * 1e-6;
  const free: number[] = [];
  for (const key of candidates) {
    if (coarseOccupied.has(key)) continue;
    const [cx, cy, cz] = key.split(',').map(Number);
    const low = [cx, cy, cz].map((cell) => Math.floor((cell * targetSize + epsilon) / fine));
    const high = [cx, cy, cz].map((cell) => Math.floor(((cell + 1) * targetSize - epsilon) / fine));
    let covered = true;
    for (let ix = low[0]; covered && ix <= high[0]; ix += 1) {
      for (let iy = low[1]; covered && iy <= high[1]; iy += 1) {
        for (let iz = low[2]; covered && iz <= high[2]; iz += 1) {
          if (!fineFree.has(cellKey(ix, iy, iz))) covered = false;
        }
      }
    }
    if (covered) free.push((cx + 0.5) * targetSize, (cy + 0.5) * targetSize, (cz + 0.5) * targetSize);
  }
  const occupied: number[] = [];
  for (const key of coarseOccupied) {
    const [cx, cy, cz] = key.split(',').map(Number);
    occupied.push((cx + 0.5) * targetSize, (cy + 0.5) * targetSize, (cz + 0.5) * targetSize);
  }
  return {
    ...snapshot,
    freeAirPositions: new Float32Array(free),
    occupiedSurfacePositions: new Float32Array(occupied),
    occupancyFineVoxelSizeMeters: targetSize,
    occupancyVoxelSizeMeters: targetSize,
  };
}

/**
 * MAX-162: the compact occupancy saved beside a session's scan, bound to
 * exactly that scan by its CRC. Until an AprilTag sensor<->scan
 * registration exists (MAX-145) `registration` stays null and the ESP32
 * keeps the constraint disabled (fail closed).
 */
export function buildSessionOccupancyArtifact(
  snapshot: Parameters<typeof encodeOccupancySolverArtifact>[0],
  scanArtifact: Uint8Array | ScanIdentity,
  createdAtMs: number,
  registration: OccupancyRegistration | null = null,
  identity?: ScanIdentity,
): Uint8Array {
  const scanIdentity = scanArtifact instanceof Uint8Array ? identity ?? createScanIdentity(scanArtifact) : scanArtifact;
  return encodeOccupancySolverArtifact(coarsenOccupancyForSolver(snapshot), {
    frameId: scanIdentity.frameId,
    createdAtMs,
    registration,
    ...(scanIdentity.sha256 === undefined ? {} : { scanSha256: scanIdentity.sha256 }),
  });
}
