/**
 * MAX-24/MAX-187: bounded binary voxel preview for the ESP32 TFT. The preview
 * deliberately keeps only EMPTY/NOT EMPTY surface voxels; interior occupied
 * cells and air are omitted because localization only distinguishes empty
 * space from occupied space.
 */

import {
  decodeOccupancySolverArtifact,
  occupancyCrc32,
  occupancyFrameIdForScan,
  type OccupancyRegistration,
} from './occupancySolverArtifact';
import type { ScanPointCloudSnapshot } from './webxrRuntime';
import { localFloorMatrixToProject } from './projectFrame';

const MAGIC = new TextEncoder().encode('GXPRV1\0\0');
const LEGACY_VERSION = 1;
const VERSION = 2;
const BASE_HEADER_BYTES = 54;
const REGISTERED_HEADER_BYTES = BASE_HEADER_BYTES + 16 * Float32Array.BYTES_PER_ELEMENT;
export const MAX_VOXEL_PREVIEW_BYTES = 64 * 1024;
export const MAX_VOXEL_PREVIEW_COUNT = 65535;
// tune by ESP32 benchmark, MAX-187
export const DEFAULT_MAX_SURFACE_VOXELS = 3000;

const FACE = {
  POS_X: 1 << 0,
  NEG_X: 1 << 1,
  POS_Y: 1 << 2,
  NEG_Y: 1 << 3,
  POS_Z: 1 << 4,
  NEG_Z: 1 << 5,
} as const;

export interface VoxelPreviewRegistration {
  geometry_computed_at_ms: number;
  /** Version 2 row-major transform from sensor Z-up to project Z-up. */
  sensor_to_scan: number[];
}

export interface VoxelPreviewArtifact {
  sourceScanCrc32: number;
  geometryComputedAtMs: bigint;
  registration: VoxelPreviewRegistration | null;
  origin: [number, number, number];
  voxelSizeMeters: number;
  dimensions: [number, number, number];
  voxelCount: number;
  indexWidth: 2 | 4;
  indices: Uint32Array;
  faceMasks: Uint8Array;
  coarseningSteps: number;
}

export interface BuildVoxelPreviewOptions {
  occupancyArtifact?: Uint8Array | null;
  registration?: OccupancyRegistration | null;
  maxSurfaceVoxels?: number;
}

export interface VoxelPreviewGrid {
  origin: [number, number, number];
  voxelSizeMeters: number;
  dimensions: [number, number, number];
  indices: Uint32Array;
  faceMasks: Uint8Array;
  coarseningSteps: number;
}

function finiteFloat32(value: number, label: string): void {
  if (!Number.isFinite(value) || !Number.isFinite(Math.fround(value))) {
    throw new Error(`Voxel preview ${label} must be a finite float32.`);
  }
}

function rigidRegistration(value: OccupancyRegistration | null | undefined): VoxelPreviewRegistration | null {
  if (value === null || value === undefined ||
      !Number.isSafeInteger(value.geometry_computed_at_ms) || value.geometry_computed_at_ms <= 0 ||
      !Array.isArray(value.sensor_to_scan) || value.sensor_to_scan.length !== 16 ||
      value.sensor_to_scan.some((entry) => !Number.isFinite(entry))) return null;
  const frame: unknown = value.frame;
  if (frame !== undefined && frame !== 'webxr_local_floor' && frame !== 'project_z_up') return null;

  const m = value.sensor_to_scan;
  if (m[12] !== 0 || m[13] !== 0 || m[14] !== 0 || m[15] !== 1) return null;
  for (let row = 0; row < 3; row += 1) {
    for (let other = 0; other < 3; other += 1) {
      const dot = m[row * 4] * m[other * 4] + m[row * 4 + 1] * m[other * 4 + 1] + m[row * 4 + 2] * m[other * 4 + 2];
      if (Math.abs(dot - (row === other ? 1 : 0)) > 1e-3) return null;
    }
  }
  const determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
  if (Math.abs(Math.abs(determinant) - 1) > 1e-3) return null;
  return {
    geometry_computed_at_ms: value.geometry_computed_at_ms,
    sensor_to_scan: frame === 'project_z_up' ? [...m] : localFloorMatrixToProject(m),
  };
}

function matchingOccupancy(
  bytes: Uint8Array | null | undefined,
  scanArtifact: Uint8Array,
) {
  if (bytes === null || bytes === undefined) return null;
  try {
    const decoded = decodeOccupancySolverArtifact(bytes);
    return decoded.header.frame_id === occupancyFrameIdForScan(scanArtifact) ? decoded : null;
  } catch {
    return null;
  }
}

function eachOccupiedSample(
  snapshot: Pick<ScanPointCloudSnapshot, 'occupiedSurfacePositions' | 'occupancyFineVoxelSizeMeters' | 'occupancyVoxelSizeMeters'>,
  occupancy: ReturnType<typeof matchingOccupancy>,
  visit: (x: number, y: number, z: number) => void,
): void {
  if (occupancy !== null) {
    const size = occupancy.header.voxel_size_m;
    for (let i = 0; i < occupancy.occupied.length; i += 3) {
      visit(
        (occupancy.occupied[i] + 0.5) * size,
        (occupancy.occupied[i + 1] + 0.5) * size,
        (occupancy.occupied[i + 2] + 0.5) * size,
      );
    }
    return;
  }

  const positions = snapshot.occupiedSurfacePositions;
  if (positions.length % 3 !== 0) throw new Error('Voxel preview surface positions must contain XYZ triples.');
  for (let i = 0; i < positions.length; i += 3) {
    visit(positions[i], positions[i + 1], positions[i + 2]);
  }
}

function key(x: number, y: number, z: number): string {
  return `${x},${y},${z}`;
}

function buildAtSize(
  snapshot: Parameters<typeof eachOccupiedSample>[0],
  occupancy: ReturnType<typeof matchingOccupancy>,
  voxelSizeMeters: number,
  coarseningSteps: number,
): VoxelPreviewGrid {
  const cells = new Map<string, [number, number, number]>();
  let minX = Infinity; let minY = Infinity; let minZ = Infinity;
  let maxX = -Infinity; let maxY = -Infinity; let maxZ = -Infinity;
  eachOccupiedSample(snapshot, occupancy, (x, y, z) => {
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) {
      throw new Error('Voxel preview surface sample is non-finite.');
    }
    const ix = Math.floor(x / voxelSizeMeters);
    const iy = Math.floor(y / voxelSizeMeters);
    const iz = Math.floor(z / voxelSizeMeters);
    if (![ix, iy, iz].every(Number.isSafeInteger)) throw new Error('Voxel preview grid coordinate exceeds the safe integer range.');
    cells.set(key(ix, iy, iz), [ix, iy, iz]);
    minX = Math.min(minX, ix); minY = Math.min(minY, iy); minZ = Math.min(minZ, iz);
    maxX = Math.max(maxX, ix); maxY = Math.max(maxY, iy); maxZ = Math.max(maxZ, iz);
  });
  if (cells.size === 0) throw new Error('Voxel preview has no occupied surface samples.');

  const dx = maxX - minX + 1;
  const dy = maxY - minY + 1;
  const dz = maxZ - minZ + 1;
  const volume = dx * dy * dz;
  if (![dx, dy, dz].every((value) => Number.isInteger(value) && value > 0 && value <= 0xffff) ||
      !Number.isSafeInteger(volume) || volume > 0x1_0000_0000) {
    throw new Error('coarsen');
  }

  const ordered: Array<{ index: number; mask: number }> = [];
  for (const [ix, iy, iz] of cells.values()) {
    let mask = 0;
    if (!cells.has(key(ix + 1, iy, iz))) mask |= FACE.POS_X;
    if (!cells.has(key(ix - 1, iy, iz))) mask |= FACE.NEG_X;
    if (!cells.has(key(ix, iy + 1, iz))) mask |= FACE.POS_Y;
    if (!cells.has(key(ix, iy - 1, iz))) mask |= FACE.NEG_Y;
    if (!cells.has(key(ix, iy, iz + 1))) mask |= FACE.POS_Z;
    if (!cells.has(key(ix, iy, iz - 1))) mask |= FACE.NEG_Z;
    if (mask !== 0) ordered.push({ index: ((iz - minZ) * dy + (iy - minY)) * dx + (ix - minX), mask });
  }
  ordered.sort((a, b) => a.index - b.index);
  const indices = new Uint32Array(ordered.length);
  const faceMasks = new Uint8Array(ordered.length);
  for (let i = 0; i < ordered.length; i += 1) {
    indices[i] = ordered[i].index;
    faceMasks[i] = ordered[i].mask;
  }
  const origin: [number, number, number] = [minX * voxelSizeMeters, minY * voxelSizeMeters, minZ * voxelSizeMeters];
  origin.forEach((value, axis) => finiteFloat32(value, `origin ${axis}`));
  finiteFloat32(voxelSizeMeters, 'voxel size');
  return {
    origin,
    voxelSizeMeters,
    dimensions: [dx, dy, dz],
    indices,
    faceMasks,
    coarseningSteps,
  };
}

export function buildVoxelPreviewGrid(
  snapshot: Parameters<typeof eachOccupiedSample>[0],
  scanArtifact: Uint8Array,
  options: BuildVoxelPreviewOptions = {},
): VoxelPreviewGrid {
  const budget = options.maxSurfaceVoxels ?? DEFAULT_MAX_SURFACE_VOXELS;
  if (!Number.isInteger(budget) || budget < 1 || budget > MAX_VOXEL_PREVIEW_COUNT) {
    throw new Error(`Voxel preview budget must be an integer from 1 to ${MAX_VOXEL_PREVIEW_COUNT}.`);
  }
  const occupancy = matchingOccupancy(options.occupancyArtifact, scanArtifact);
  const registration = rigidRegistration(options.registration ?? occupancy?.header.registration);
  const initialSize = occupancy?.header.voxel_size_m ??
    snapshot.occupancyFineVoxelSizeMeters ?? snapshot.occupancyVoxelSizeMeters;
  if (!(initialSize > 0) || !Number.isFinite(initialSize)) throw new Error('Voxel preview needs a positive scan voxel size.');

  let size = initialSize;
  for (let steps = 0; steps < 64; steps += 1) {
    let grid: VoxelPreviewGrid;
    try {
      grid = buildAtSize(snapshot, occupancy, size, steps);
    } catch (reason) {
      if (!(reason instanceof Error) || reason.message !== 'coarsen') throw reason;
      size *= 2;
      continue;
    }
    const indexWidth = grid.dimensions[0] * grid.dimensions[1] * grid.dimensions[2] <= 0x10000 ? 2 : 4;
    const encodedBytes = (grid.indices.length * (indexWidth + 1)) +
      (registration === null ? BASE_HEADER_BYTES : REGISTERED_HEADER_BYTES) + 4;
    if (grid.indices.length <= budget && grid.indices.length <= MAX_VOXEL_PREVIEW_COUNT && encodedBytes <= MAX_VOXEL_PREVIEW_BYTES) {
      return grid;
    }
    size *= 2;
    if (!Number.isFinite(size)) break;
  }
  throw new Error('Voxel preview could not fit the ESP32 surface budget.');
}

export function buildVoxelPreviewArtifact(
  snapshot: Parameters<typeof buildVoxelPreviewGrid>[0],
  scanArtifact: Uint8Array,
  options: BuildVoxelPreviewOptions = {},
): Uint8Array {
  const grid = buildVoxelPreviewGrid(snapshot, scanArtifact, options);
  const occupancy = matchingOccupancy(options.occupancyArtifact, scanArtifact);
  const registration = rigidRegistration(options.registration ?? occupancy?.header.registration);
  return encodeVoxelPreviewArtifact({
    sourceScanCrc32: occupancyCrc32(scanArtifact),
    geometryComputedAtMs: BigInt(registration?.geometry_computed_at_ms ?? 0),
    registration,
    ...grid,
    voxelCount: grid.indices.length,
    indexWidth: grid.dimensions[0] * grid.dimensions[1] * grid.dimensions[2] <= 0x10000 ? 2 : 4,
  });
}

export function encodeVoxelPreviewArtifact(artifact: VoxelPreviewArtifact): Uint8Array {
  const { dimensions, indices, faceMasks } = artifact;
  if (!Number.isInteger(artifact.sourceScanCrc32) || artifact.sourceScanCrc32 < 0 || artifact.sourceScanCrc32 > 0xffffffff) throw new Error('Voxel preview source CRC is invalid.');
  if (artifact.voxelCount !== indices.length || faceMasks.length !== indices.length || artifact.voxelCount > MAX_VOXEL_PREVIEW_COUNT) throw new Error('Voxel preview voxel count is inconsistent.');
  if (artifact.coarseningSteps < 0 || artifact.coarseningSteps > 0xffff || !Number.isInteger(artifact.coarseningSteps)) throw new Error('Voxel preview coarsening count is invalid.');
  if (dimensions.length !== 3 || dimensions.some((value) => !Number.isInteger(value) || value <= 0 || value > 0xffff)) throw new Error('Voxel preview dimensions are invalid.');
  const volume = dimensions[0] * dimensions[1] * dimensions[2];
  if (!Number.isSafeInteger(volume) || volume > 0x1_0000_0000) throw new Error('Voxel preview grid volume is invalid.');
  const expectedWidth = volume <= 0x10000 ? 2 : 4;
  if (artifact.indexWidth !== expectedWidth) throw new Error('Voxel preview index width does not match its dimensions.');
  if (artifact.origin.length !== 3) throw new Error('Voxel preview origin is invalid.');
  artifact.origin.forEach((value, axis) => finiteFloat32(value, `origin ${axis}`));
  finiteFloat32(artifact.voxelSizeMeters, 'voxel size');
  if (!(artifact.voxelSizeMeters > 0)) throw new Error('Voxel preview voxel size must be positive.');
  if (artifact.registration === null) {
    if (artifact.geometryComputedAtMs !== 0n) throw new Error('Unregistered voxel preview must have a zero geometry timestamp.');
  } else {
    const checked = rigidRegistration({
      frame: 'project_z_up',
      source: 'preview',
      geometry_computed_at_ms: Number(artifact.geometryComputedAtMs),
      sensor_to_scan: artifact.registration.sensor_to_scan,
      residual_m: 0,
    });
    if (checked === null || BigInt(checked.geometry_computed_at_ms) !== artifact.geometryComputedAtMs) throw new Error('Voxel preview registration is invalid.');
  }

  const headerBytes = artifact.registration === null ? BASE_HEADER_BYTES : REGISTERED_HEADER_BYTES;
  const totalBytes = headerBytes + artifact.voxelCount * (artifact.indexWidth + 1) + 4;
  if (totalBytes > MAX_VOXEL_PREVIEW_BYTES) throw new Error('Voxel preview exceeds the 64 KiB device limit.');
  const result = new Uint8Array(totalBytes);
  result.set(MAGIC, 0);
  const view = new DataView(result.buffer);
  view.setUint16(8, artifact.registration === null ? LEGACY_VERSION : VERSION, true);
  view.setUint16(10, headerBytes, true);
  view.setUint32(12, artifact.sourceScanCrc32, true);
  view.setBigUint64(16, artifact.geometryComputedAtMs, true);
  view.setUint8(24, artifact.registration === null ? 0 : 1);
  view.setUint8(25, artifact.indexWidth);
  for (let axis = 0; axis < 3; axis += 1) view.setUint16(26 + axis * 2, dimensions[axis], true);
  for (let axis = 0; axis < 3; axis += 1) view.setFloat32(32 + axis * 4, artifact.origin[axis], true);
  view.setFloat32(44, artifact.voxelSizeMeters, true);
  view.setUint32(48, artifact.voxelCount, true);
  view.setUint16(52, artifact.coarseningSteps, true);
  if (artifact.registration !== null) {
    for (let i = 0; i < 16; i += 1) view.setFloat32(BASE_HEADER_BYTES + i * 4, artifact.registration.sensor_to_scan[i], true);
  }
  let offset = headerBytes;
  let previous = -1;
  for (let i = 0; i < artifact.voxelCount; i += 1) {
    const index = indices[i];
    const mask = faceMasks[i];
    if (index >= volume || index <= previous) throw new Error('Voxel preview indices must be sorted, unique, and in range.');
    if ((mask & 0xc0) !== 0 || mask === 0) throw new Error('Voxel preview face mask is invalid.');
    if (artifact.indexWidth === 2) view.setUint16(offset, index, true);
    else view.setUint32(offset, index, true);
    offset += artifact.indexWidth;
    previous = index;
  }
  result.set(faceMasks, offset);
  view.setUint32(totalBytes - 4, occupancyCrc32(result.subarray(0, totalBytes - 4)), true);
  return result;
}

export function decodeVoxelPreviewArtifact(source: ArrayBuffer | Uint8Array, expectedScanArtifact?: Uint8Array): VoxelPreviewArtifact {
  const bytes = source instanceof Uint8Array ? source : new Uint8Array(source);
  if (bytes.byteLength < BASE_HEADER_BYTES + 4 || bytes.byteLength > MAX_VOXEL_PREVIEW_BYTES) throw new Error('Invalid voxel preview size.');
  for (let i = 0; i < MAGIC.length; i += 1) if (bytes[i] !== MAGIC[i]) throw new Error('Voxel preview magic mismatch.');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint16(8, true);
  if (version !== LEGACY_VERSION && version !== VERSION) throw new Error('Unsupported voxel preview version.');
  const headerBytes = view.getUint16(10, true);
  const registered = view.getUint8(24);
  if (registered > 1 || headerBytes !== (registered ? REGISTERED_HEADER_BYTES : BASE_HEADER_BYTES)) throw new Error('Voxel preview header length is invalid.');
  if (occupancyCrc32(bytes.subarray(0, bytes.byteLength - 4)) !== view.getUint32(bytes.byteLength - 4, true)) throw new Error('Voxel preview CRC mismatch.');

  const dimensions: [number, number, number] = [view.getUint16(26, true), view.getUint16(28, true), view.getUint16(30, true)];
  if (dimensions.some((value) => value === 0)) throw new Error('Voxel preview dimensions are invalid.');
  const volume = dimensions[0] * dimensions[1] * dimensions[2];
  if (!Number.isSafeInteger(volume) || volume > 0x1_0000_0000) throw new Error('Voxel preview grid volume is invalid.');
  const indexWidth = view.getUint8(25);
  const expectedWidth = volume <= 0x10000 ? 2 : 4;
  if (indexWidth !== expectedWidth) throw new Error('Voxel preview index width is invalid.');
  const voxelCount = view.getUint32(48, true);
  if (voxelCount > MAX_VOXEL_PREVIEW_COUNT || voxelCount > volume) throw new Error('Voxel preview count exceeds the grid or hard limit.');
  if (headerBytes + voxelCount * (indexWidth + 1) + 4 !== bytes.byteLength) throw new Error('Voxel preview counts disagree with its length.');

  const encodedGeometryComputedAtMs = view.getBigUint64(16, true);
  const origin: [number, number, number] = [view.getFloat32(32, true), view.getFloat32(36, true), view.getFloat32(40, true)];
  const voxelSizeMeters = view.getFloat32(44, true);
  if (!origin.every(Number.isFinite) || !Number.isFinite(voxelSizeMeters) || voxelSizeMeters <= 0) throw new Error('Voxel preview geometry is invalid.');
  let registration: VoxelPreviewRegistration | null = null;
  if (registered) {
    const sensor_to_scan = Array.from({ length: 16 }, (_, i) => view.getFloat32(BASE_HEADER_BYTES + i * 4, true));
    const checked = rigidRegistration({ frame: 'project_z_up', source: 'preview', geometry_computed_at_ms: Number(encodedGeometryComputedAtMs), sensor_to_scan, residual_m: 0 });
    if (checked === null || BigInt(checked.geometry_computed_at_ms) !== encodedGeometryComputedAtMs) throw new Error('Voxel preview registration is invalid.');
    if (version === VERSION) registration = checked;
  } else if (encodedGeometryComputedAtMs !== 0n) throw new Error('Unregistered voxel preview has a geometry timestamp.');
  const geometryComputedAtMs = registration === null ? 0n : encodedGeometryComputedAtMs;

  const indices = new Uint32Array(voxelCount);
  const faceMasks = new Uint8Array(voxelCount);
  let offset = headerBytes;
  let previous = -1;
  for (let i = 0; i < voxelCount; i += 1) {
    const index = indexWidth === 2 ? view.getUint16(offset, true) : view.getUint32(offset, true);
    offset += indexWidth;
    if (index >= volume) throw new Error('Voxel preview index is out of range.');
    if (index <= previous) throw new Error('Voxel preview indices are unsorted or duplicated.');
    indices[i] = index;
    previous = index;
  }
  for (let i = 0; i < voxelCount; i += 1) {
    const mask = view.getUint8(offset + i);
    if ((mask & 0xc0) !== 0 || mask === 0) throw new Error('Voxel preview face mask is invalid.');
    faceMasks[i] = mask;
  }
  if (expectedScanArtifact !== undefined && occupancyCrc32(expectedScanArtifact) !== view.getUint32(12, true)) throw new Error('Voxel preview is stale for the saved scan.');

  return {
    sourceScanCrc32: view.getUint32(12, true),
    geometryComputedAtMs,
    registration,
    origin,
    voxelSizeMeters,
    dimensions,
    voxelCount,
    indexWidth,
    indices,
    faceMasks,
    coarseningSteps: view.getUint16(52, true),
  };
}
