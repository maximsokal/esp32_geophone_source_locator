/**
 * The ESP32 TFT preview (GXPRV1) as 3D geometry for the existing post-scan viewport (issue #44).
 *
 * The geometry is derived ONLY from a decoded artifact, i.e. from the very bytes that were (or will be) sent to the ESP32: the cell set, the origin,
 * the voxel size and the exposed-face masks are used as they are, never re-voxelised from the scan. Only the faces the mask marks as exposed are
 * built, as one batched indexed-free triangle soup, so thousands of cells are a single buffer geometry and not thousands of scene objects.
 *
 * Frames: the artifact is in the project frame (Z up); `toViewport` maps it to the Three.js scene (Y up) with the viewport's own centre, exactly as
 * every other scan layer. A version 2 artifact's registration matrix is metadata for the device (sensor frame -> project frame); the cell grid itself
 * is already in the project frame, so it is NOT applied again here.
 *
 * Cell geometry (matches `buildAtSize` in voxelPreview.ts): cell (ix,iy,iz) spans [origin + i*size, origin + (i+1)*size] on each axis; its index is
 * ((iz*dy)+iy)*dx+ix. Face bits: +X, -X, +Y, -Y, +Z, -Z = bits 0..5.
 */

import type { VoxelPreviewArtifact } from './voxelPreview';
import { toViewport } from './scanViewportScene';

export const VOXEL_FACE_BITS = ['+X', '-X', '+Y', '-Y', '+Z', '-Z'] as const;

export interface VoxelCell {
  /** Position in the artifact's cell list (the n-th transmitted cell). */
  readonly ordinal: number;
  readonly index: number;
  readonly grid: readonly [number, number, number];
  readonly faceMask: number;
  /** Minimum corner and centre in the project frame (Z up), metres. */
  readonly minCorner: readonly [number, number, number];
  readonly center: readonly [number, number, number];
}

export interface VoxelSceneCentre { x: number; y: number; z: number }

export interface Esp32VoxelGeometry {
  /** Triangle soup in viewport coordinates, 3 floats per vertex, 3 vertices per triangle. */
  positions: Float32Array;
  /** One outward unit normal per vertex (flat shading). */
  normals: Float32Array;
  /** For each triangle, the ordinal of the transmitted cell it belongs to (picking). */
  triangleCell: Uint32Array;
  /** For each triangle, which face bit (0..5) it belongs to. */
  triangleFace: Uint8Array;
  cellCount: number;
  faceCount: number;
  triangleCount: number;
  /** Viewport-frame axis-aligned bounds of everything drawn. */
  bounds: { min: [number, number, number]; max: [number, number, number] };
}

export function decodeVoxelCell(artifact: VoxelPreviewArtifact, ordinal: number): VoxelCell {
  if (!Number.isInteger(ordinal) || ordinal < 0 || ordinal >= artifact.voxelCount) throw new RangeError(`Voxel ordinal ${ordinal} is outside the artifact.`);
  const [dx, dy] = artifact.dimensions;
  const index = artifact.indices[ordinal];
  const ix = index % dx;
  const iy = Math.floor(index / dx) % dy;
  const iz = Math.floor(index / (dx * dy));
  const size = artifact.voxelSizeMeters;
  const minCorner: [number, number, number] = [artifact.origin[0] + ix * size, artifact.origin[1] + iy * size, artifact.origin[2] + iz * size];
  return {
    ordinal,
    index,
    grid: [ix, iy, iz],
    faceMask: artifact.faceMasks[ordinal],
    minCorner,
    center: [minCorner[0] + size / 2, minCorner[1] + size / 2, minCorner[2] + size / 2],
  };
}

export function exposedFaceCount(artifact: VoxelPreviewArtifact): number {
  let total = 0;
  for (let i = 0; i < artifact.voxelCount; i += 1) {
    for (let bit = 0; bit < 6; bit += 1) if ((artifact.faceMasks[i] & (1 << bit)) !== 0) total += 1;
  }
  return total;
}

const AXIS_OF_BIT = [0, 0, 1, 1, 2, 2] as const;
const POSITIVE_OF_BIT = [true, false, true, false, true, false] as const;

export function buildEsp32VoxelGeometry(artifact: VoxelPreviewArtifact, centre: VoxelSceneCentre): Esp32VoxelGeometry {
  const minCorners = new Float64Array(artifact.voxelCount * 3);
  for (let ordinal = 0; ordinal < artifact.voxelCount; ordinal += 1) {
    const cell = decodeVoxelCell(artifact, ordinal);
    minCorners.set(cell.minCorner, ordinal * 3);
  }
  return buildCellFaceGeometry(minCorners, artifact.faceMasks, artifact.voxelSizeMeters, centre);
}

/**
 * Exposed faces of axis-aligned cubes of one size (project frame, Z up) as one triangle soup in viewport coordinates. `minCorners` holds three
 * numbers per cell (the minimum corner, metres) and `faceMasks` one exposed-face mask per cell (bit order as VOXEL_FACE_BITS). The triangle's
 * `triangleCell` is the position of its cell in `minCorners`. Shared by the TFT preview and the solver occupancy so both draw the same way.
 */
export function buildCellFaceGeometry(minCorners: Float64Array, faceMasks: Uint8Array, size: number, centre: VoxelSceneCentre): Esp32VoxelGeometry {
  const cellCount = faceMasks.length;
  let faces = 0;
  for (let i = 0; i < cellCount; i += 1) {
    for (let bit = 0; bit < 6; bit += 1) if ((faceMasks[i] & (1 << bit)) !== 0) faces += 1;
  }
  const triangleCount = faces * 2;
  const positions = new Float32Array(triangleCount * 9);
  const normals = new Float32Array(triangleCount * 9);
  const triangleCell = new Uint32Array(triangleCount);
  const triangleFace = new Uint8Array(triangleCount);
  const min: [number, number, number] = [Infinity, Infinity, Infinity];
  const max: [number, number, number] = [-Infinity, -Infinity, -Infinity];
  let triangle = 0;

  const emit = (vertex: [number, number, number], normal: readonly [number, number, number], slot: number): void => {
    const view = toViewport(vertex[0], vertex[1], vertex[2], centre.x, centre.y, centre.z);
    const normalView = toViewport(normal[0], normal[1], normal[2], 0, 0, 0);
    positions.set(view, slot);
    normals.set(normalView, slot);
    for (let axis = 0; axis < 3; axis += 1) {
      if (view[axis] < min[axis]) min[axis] = view[axis];
      if (view[axis] > max[axis]) max[axis] = view[axis];
    }
  };

  for (let ordinal = 0; ordinal < cellCount; ordinal += 1) {
    const minCorner = [minCorners[ordinal * 3], minCorners[ordinal * 3 + 1], minCorners[ordinal * 3 + 2]];
    for (let bit = 0; bit < 6; bit += 1) {
      if ((faceMasks[ordinal] & (1 << bit)) === 0) continue;
      const axis = AXIS_OF_BIT[bit];
      const positive = POSITIVE_OF_BIT[bit];
      const u = (axis + 1) % 3;
      const v = (axis + 2) % 3;
      const normal: [number, number, number] = [0, 0, 0];
      normal[axis] = positive ? 1 : -1;
      const origin: [number, number, number] = [minCorner[0], minCorner[1], minCorner[2]];
      if (positive) origin[axis] += size;
      const corner = (du: number, dv: number): [number, number, number] => {
        const point: [number, number, number] = [origin[0], origin[1], origin[2]];
        point[u] += du * size;
        point[v] += dv * size;
        return point;
      };
      /* u x v = +axis for the cyclic axes, so (0,0),(1,0),(1,1),(0,1) is counter-clockwise seen from +axis; reverse it for the -axis face. */
      const quad = positive
        ? [corner(0, 0), corner(1, 0), corner(1, 1), corner(0, 1)]
        : [corner(0, 0), corner(0, 1), corner(1, 1), corner(1, 0)];
      for (const [a, b, c] of [[0, 1, 2], [0, 2, 3]] as const) {
        const base = triangle * 9;
        emit(quad[a], normal, base);
        emit(quad[b], normal, base + 3);
        emit(quad[c], normal, base + 6);
        triangleCell[triangle] = ordinal;
        triangleFace[triangle] = bit;
        triangle += 1;
      }
    }
  }
  if (triangleCount === 0) {
    min.fill(0);
    max.fill(0);
  }
  return { positions, normals, triangleCell, triangleFace, cellCount, faceCount: faces, triangleCount, bounds: { min, max } };
}
