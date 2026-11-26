/*
 * The translucent surface drawn over the camera while a scan runs (phone run 2026-11-22: "show where the mesh is already forming"). Pure: a bounded set of
 * the live surface points in, WebXR-frame triangles out.
 *
 * Every measured point becomes a small oriented patch (two triangles, a few centimetres wide, lying in the plane given by the point's measured normal).
 * Neighbouring patches overlap, so measured surface reads as one translucent skin and unmeasured space stays empty. An earlier version triangulated the
 * points like the post-scan preview does; on live data, which is sparse and still settling, that kept only fragments of components and left the
 * surface as unconnected islands (phone run 2026-11-26: 31 to 56 islands covering 26 to 44% of the points). Patches drop nothing, need no neighbour
 * search and cost milliseconds, so the preview stays cheap on a phone that is busy with depth fusion.
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
  /** Points that became a patch (a point without a usable normal does not). */
  acceptedPointCount: number;
}

export const LIVE_MESH_MAX_POINTS = 4000;
/** Two triangles per patch. */
export const LIVE_MESH_MAX_TRIANGLES = LIVE_MESH_MAX_POINTS * 2;
/** Fewer points than this is not a surface yet. */
export const LIVE_MESH_MIN_POINTS = 16;
/** Half the side of a patch, in voxels: a patch is a little wider than the spacing of the points, so neighbours overlap. */
export const LIVE_MESH_PATCH_HALF_VOXELS = 0.75;

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
  const half = Math.min(0.06, Math.max(0.015, Math.abs(limited.voxelSizeMeters) * LIVE_MESH_PATCH_HALF_VOXELS || 0.03));
  const capacity = Math.min(count, Math.floor(Math.max(0, maxTriangles) / 2));
  const out = new Float32Array(capacity * 18);
  let patches = 0;
  if (count >= LIVE_MESH_MIN_POINTS) {
    for (let i = 0; i < count && patches < capacity; i += 1) {
      const px = limited.positions[i * 3]; const py = limited.positions[i * 3 + 1]; const pz = limited.positions[i * 3 + 2];
      let nx = limited.normals[i * 3]; let ny = limited.normals[i * 3 + 1]; let nz = limited.normals[i * 3 + 2];
      const length = Math.hypot(nx, ny, nz);
      if (![px, py, pz, length].every(Number.isFinite) || length < 1e-6) continue;
      nx /= length; ny /= length; nz /= length;
      /* two tangents: cross the normal with the axis it is least aligned with */
      const ax = Math.abs(nx); const ay = Math.abs(ny); const az = Math.abs(nz);
      const ux = ax <= ay && ax <= az ? 1 : 0; const uy = ay < ax && ay <= az ? 1 : 0; const uz = ux === 0 && uy === 0 ? 1 : 0;
      let t1x = ny * uz - nz * uy; let t1y = nz * ux - nx * uz; let t1z = nx * uy - ny * ux;
      const t1 = Math.hypot(t1x, t1y, t1z);
      t1x = t1x / t1 * half; t1y = t1y / t1 * half; t1z = t1z / t1 * half;
      const t2x = ny * t1z - nz * t1y; const t2y = nz * t1x - nx * t1z; const t2z = nx * t1y - ny * t1x;      /* n x t1: already `half` long */
      const corners = [
        [px - t1x - t2x, py - t1y - t2y, pz - t1z - t2z], [px + t1x - t2x, py + t1y - t2y, pz + t1z - t2z],
        [px + t1x + t2x, py + t1y + t2y, pz + t1z + t2z], [px - t1x + t2x, py - t1y + t2y, pz - t1z + t2z],
      ];
      /* project (x, y, z) -> WebXR local floor (x, z, -y) */
      const triangles = [[0, 1, 2], [0, 2, 3]];
      for (let t = 0; t < 2; t += 1) for (let c = 0; c < 3; c += 1) {
        const corner = corners[triangles[t][c]];
        const o = patches * 18 + t * 9 + c * 3;
        out[o] = corner[0]; out[o + 1] = corner[2]; out[o + 2] = -corner[1];
      }
      patches += 1;
    }
  }
  return { trianglesXr: out.slice(0, patches * 18), triangleCount: patches * 2, sourcePointCount: count, acceptedPointCount: patches };
}
