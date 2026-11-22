import { buildSurfaceMeshPreview } from './meshPreview';

/*
 * The translucent mesh drawn over the camera while a scan runs (phone run 2026-11-22: "show where the mesh is already forming"). Pure: a bounded set of
 * the live surface points in, WebXR-frame triangles out. The same surfel triangulation the post-scan preview uses, capped so the GPU upload and the
 * worker time stay small on a phone that is already busy with depth fusion.
 */
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
  acceptedPointCount: number;
}

export const LIVE_MESH_MAX_POINTS = 4000;
export const LIVE_MESH_MAX_TRIANGLES = 6000;

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
  const mesh = buildSurfaceMeshPreview(limited);
  const triangleCount = Math.min(mesh.triangleCount, Math.max(0, Math.floor(maxTriangles)));
  const source = mesh.trianglePositions;
  const trianglesXr = new Float32Array(triangleCount * 9);
  for (let vertex = 0; vertex < triangleCount * 3; vertex += 1) {
    const o = vertex * 3;
    trianglesXr[o] = source[o]; trianglesXr[o + 1] = source[o + 2]; trianglesXr[o + 2] = -source[o + 1];      /* project (x, y, z) -> WebXR local floor (x, z, -y) */
  }
  return { trianglesXr, triangleCount, sourcePointCount: mesh.sourcePointCount, acceptedPointCount: mesh.acceptedPointCount };
}
