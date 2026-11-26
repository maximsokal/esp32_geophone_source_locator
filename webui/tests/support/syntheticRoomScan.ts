import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';

/*
 * A synthetic, fully measured room corner: a 2.4 x 2.4 m floor and two 1.2 m walls, sampled every 5 cm with exact normals. Public and made up: it
 * reproduces the SHAPES the post-scan pipeline works on (large horizontal surface, vertical surfaces, a shared inside corner) without any real place.
 */
export interface SyntheticRoomOptions {
  /** Edge of the floor in metres (the walls are as long). */
  size?: number;
  wallHeight?: number;
  spacing?: number;
  /** Shift of the whole room, to make two different scans. */
  offset?: readonly [number, number, number];
}

export function syntheticRoomScan(options: SyntheticRoomOptions = {}): ScanPointCloudSnapshot {
  const size = options.size ?? 2.4; const height = options.wallHeight ?? 1.2; const step = options.spacing ?? 0.05;
  const [ox, oy, oz] = options.offset ?? [0, 0, 0];
  const points: number[] = []; const normals: number[] = [];
  const count = Math.round(size / step);
  for (let i = 0; i <= count; i += 1) for (let j = 0; j <= count; j += 1) { points.push(ox + i * step, oy + j * step, oz); normals.push(0, 0, 1); }
  const rise = Math.round(height / step);
  for (let i = 0; i <= count; i += 1) for (let k = 1; k <= rise; k += 1) { points.push(ox, oy + i * step, oz + k * step); normals.push(1, 0, 0); }
  for (let i = 1; i <= count; i += 1) for (let k = 1; k <= rise; k += 1) { points.push(ox + i * step, oy, oz + k * step); normals.push(0, 1, 0); }
  const n = points.length / 3;
  const positions = Float32Array.from(points);
  const normalArray = Float32Array.from(normals);
  const min: [number, number, number] = [ox, oy, oz]; const max: [number, number, number] = [ox + size, oy + size, oz + height];
  const voxel = 0.18;
  const occupied: number[] = []; const free: number[] = []; const unknown: number[] = [];
  for (let x = 0; x < size; x += voxel) for (let y = 0; y < size; y += voxel) {
    occupied.push(ox + x + voxel / 2, oy + y + voxel / 2, oz);
    for (let z = voxel; z < height; z += voxel) free.push(ox + x + voxel / 2 + voxel, oy + y + voxel / 2 + voxel, oz + z);
  }
  for (let x = voxel; x < size; x += voxel * 3) unknown.push(ox + x, oy + size + voxel, oz + voxel);
  const base = makeValidScanSnapshot();
  return {
    ...base,
    positions, normals: normalArray, confidence: new Float32Array(n).fill(0.92), normalConfidence: new Float32Array(n).fill(0.9), pointCount: n,
    rawPointCount: n, bounds: { min, max }, fullBounds: { min, max },
    coverageCellCount: n, promotedPointCount: n, readyCoverageCellCount: n,
    occupiedSurfaceVoxelCount: occupied.length / 3, occupiedSurfacePositions: Float32Array.from(occupied),
    knownFreeVoxelCount: free.length / 3, freeAirPositions: Float32Array.from(free),
    unknownCandidateVoxelCount: unknown.length / 3, unknownCandidatePositions: Float32Array.from(unknown),
    occupancyEnvelopeBounds: { min, max },
    tsdfPositions: positions, tsdfNormals: normalArray, tsdfConfidence: new Float32Array(n).fill(0.92), tsdfResidualMeters: new Float32Array(n).fill(0.004),
    tsdfNormalConfidence: new Float32Array(n).fill(0.9), tsdfCoverageConfidence: new Float32Array(n).fill(0.92), tsdfCoverageReady: new Uint8Array(n).fill(1), tsdfPointCount: n,
    manualControlPoints: [], operatorEdges: [],
  };
}
