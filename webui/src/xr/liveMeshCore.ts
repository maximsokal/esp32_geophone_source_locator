/*
 * The translucent surface drawn over the camera while a scan runs (phone run 2026-11-22: "show where the mesh is already forming"). Pure: a bounded set of
 * the live surface points in, WebXR-frame triangles out.
 *
 * The surface is a connected triangle mesh from naive surface nets over the points (liveSurfaceNets.ts): milliseconds to build, one skin that hugs the
 * measured points. Two earlier attempts failed on the phone: triangulating the points like the post-scan preview left 31 to 56 unconnected islands
 * (2026-11-26 first run), and one patch per point filled the screen with overlapping squares (2026-11-26 second run). A real TSDF reconstruction was
 * measured at 0.8 to 4 s on a desktop, far too slow to repeat while the phone is also fusing depth.
 *
 * Rebuilding only the changed blocks (Voxblox style) is NOT used (decided 2026-11-28). Between consecutive builds of two replayed scans a median 95-96 % of
 * the 8 x 8 x 8 cell blocks changed and no build had 10 % or less, so almost everything would be rebuilt anyway (the build takes 6-13 ms). Two causes:
 * limitLiveMeshInput takes an even-stride subsample of the whole cloud, which picks different points whenever the cloud grows, and the Surface Nets
 * lattice starts at the lowest point, so it shifts when that point moves. It becomes worth measuring again only after the sample is stable per place and the
 * lattice (and with it the blocks) is anchored in space.
 */
import { buildSurfaceNets } from './liveSurfaceNets';

export interface LiveMeshInput {
  /** Project frame (Z up), metres. */
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
}

export interface LiveMeshResult {
  /** Triangle soup, 9 floats per triangle, in the active WebXR reference-space frame (x, z, -y of the project frame). */
  trianglesXr: Float32Array;
  triangleCount: number;
  sourcePointCount: number;
  /** Lattice cells that hold measured points (the skin is built around them). */
  acceptedPointCount: number;
}

export const LIVE_MESH_MAX_POINTS = 4000;
export const LIVE_MESH_MAX_TRIANGLES = 20000;
/** Fewer points than this is not a surface yet. */
export const LIVE_MESH_MIN_POINTS = 16;
/** Lattice spacing in voxels of the scan (a 4 cm voxel gives 8 cm cells), kept within a sensible range. */
export const LIVE_MESH_CELL_VOXELS = 2;
export const LIVE_MESH_MIN_CELL_METERS = 0.06;
export const LIVE_MESH_MAX_CELL_METERS = 0.1;
/** When the mesh would exceed the triangle budget the cells are made coarser, up to this many times (never cut: a cut leaves bands). */
export const LIVE_MESH_COARSEN_FACTOR = 1.3;
export const LIVE_MESH_MAX_COARSENINGS = 4;

/** Even subsample of at most `limit` points, keeping the three arrays aligned. */
export function limitLiveMeshInput(input: LiveMeshInput, limit = LIVE_MESH_MAX_POINTS): LiveMeshInput {
  const count = Math.min(Math.floor(input.positions.length / 3), Math.floor(input.normals.length / 3), input.confidence.length);
  if (count <= limit) return { ...input, positions: input.positions.subarray(0, count * 3), normals: input.normals.subarray(0, count * 3), confidence: input.confidence.subarray(0, count) };
  const stride = count / limit;
  const positions = new Float32Array(limit * 3); const normals = new Float32Array(limit * 3); const confidence = new Float32Array(limit);
  for (let i = 0; i < limit; i += 1) {
    const source = Math.min(count - 1, Math.floor(i * stride));
    positions.set(input.positions.subarray(source * 3, source * 3 + 3), i * 3);
    normals.set(input.normals.subarray(source * 3, source * 3 + 3), i * 3);
    confidence[i] = input.confidence[source];
  }
  return { ...input, positions, normals, confidence };
}

export function buildLiveMesh(input: LiveMeshInput, maxTriangles = LIVE_MESH_MAX_TRIANGLES): LiveMeshResult {
  const limited = limitLiveMeshInput(input);
  const count = limited.confidence.length;
  const empty: LiveMeshResult = { trianglesXr: new Float32Array(0), triangleCount: 0, sourcePointCount: count, acceptedPointCount: 0 };
  if (count < LIVE_MESH_MIN_POINTS) return empty;
  let cell = Math.min(LIVE_MESH_MAX_CELL_METERS, Math.max(LIVE_MESH_MIN_CELL_METERS, Math.abs(limited.voxelSizeMeters) * LIVE_MESH_CELL_VOXELS || LIVE_MESH_MIN_CELL_METERS));
  const budget = Math.max(0, Math.floor(maxTriangles));
  let nets = buildSurfaceNets({ positions: limited.positions, pointCount: count, cellMeters: cell });
  for (let attempt = 0; attempt < LIVE_MESH_MAX_COARSENINGS && nets.triangleCount > budget; attempt += 1) {
    cell *= LIVE_MESH_COARSEN_FACTOR;
    nets = buildSurfaceNets({ positions: limited.positions, pointCount: count, cellMeters: cell });
  }
  const triangleCount = Math.min(nets.triangleCount, budget);
  const trianglesXr = new Float32Array(triangleCount * 9);
  for (let vertex = 0; vertex < triangleCount * 3; vertex += 1) {
    const o = vertex * 3;
    /* project (x, y, z) -> WebXR local floor (x, z, -y) */
    trianglesXr[o] = nets.triangles[o]; trianglesXr[o + 1] = nets.triangles[o + 2]; trianglesXr[o + 2] = -nets.triangles[o + 1];
  }
  return { trianglesXr, triangleCount, sourcePointCount: count, acceptedPointCount: nets.occupiedCells };
}
