/*
 * Measurements of a triangle-soup mesh against measured points or a known surface. Used by the reconstruction regression tests (public synthetic scenes and
 * the local real-scan replay). Everything is computed on the mesh AS DISPLAYED: after the last operation that changes geometry.
 */

export type Vec3 = readonly [number, number, number];

export interface TopologyMetrics {
  triangles: number;
  vertices: number;
  components: number;
  componentSizes: number[];
  largestComponentRatio: number;
  boundaryEdges: number;
  boundaryEdgeLengthMeters: number;
  nonManifoldEdges: number;
  degenerateTriangles: number;
}

/** Welds the soup by quantised corner position (default 4 mm) and measures its topology. */
export function topologyMetrics(soup: Float32Array, quantum = 0.004): TopologyMetrics {
  const ids = new Map<string, number>();
  const positions: number[] = [];
  const parent: number[] = [];
  const find = (a: number): number => { while (parent[a] !== a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
  const id = (o: number): number => {
    const key = `${Math.round(soup[o] / quantum)},${Math.round(soup[o + 1] / quantum)},${Math.round(soup[o + 2] / quantum)}`;
    let v = ids.get(key);
    if (v === undefined) { v = ids.size; ids.set(key, v); parent.push(v); positions.push(soup[o], soup[o + 1], soup[o + 2]); }
    return v;
  };
  const edges = new Map<string, number>();
  const firstVertex: number[] = [];
  let degenerate = 0;
  const triangles = Math.floor(soup.length / 9);
  for (let t = 0; t < triangles; t += 1) {
    const v = [id(t * 9), id(t * 9 + 3), id(t * 9 + 6)];
    if (v[0] === v[1] || v[1] === v[2] || v[0] === v[2]) { degenerate += 1; continue; }
    parent[find(v[0])] = find(v[1]); parent[find(v[1])] = find(v[2]);
    firstVertex.push(v[0]);
    for (let e = 0; e < 3; e += 1) { const a = v[e]; const b = v[(e + 1) % 3]; const k = a < b ? `${a}_${b}` : `${b}_${a}`; edges.set(k, (edges.get(k) ?? 0) + 1); }
  }
  const sizes = new Map<number, number>();
  for (const v of firstVertex) sizes.set(find(v), (sizes.get(find(v)) ?? 0) + 1);
  const componentSizes = [...sizes.values()].sort((a, b) => b - a);
  let boundary = 0; let boundaryLength = 0; let nonManifold = 0;
  for (const [key, uses] of edges) {
    if (uses === 1) {
      boundary += 1;
      const [a, b] = key.split('_').map(Number);
      boundaryLength += Math.hypot(positions[a * 3] - positions[b * 3], positions[a * 3 + 1] - positions[b * 3 + 1], positions[a * 3 + 2] - positions[b * 3 + 2]);
    } else if (uses > 2) nonManifold += 1;
  }
  const live = firstVertex.length;
  return {
    triangles: live, vertices: ids.size, components: componentSizes.length, componentSizes,
    largestComponentRatio: live === 0 ? 0 : componentSizes[0] / live, boundaryEdges: boundary, boundaryEdgeLengthMeters: boundaryLength,
    nonManifoldEdges: nonManifold, degenerateTriangles: degenerate,
  };
}

/** Points sampled on the triangles: centroid, corners, edge midpoints and a few interior points; enough to measure surface distance without a full mesh sampler. */
export function sampleMesh(soup: Float32Array, perTriangle: 'light' | 'dense' = 'light'): Float32Array {
  const weights: number[][] = perTriangle === 'light'
    ? [[1 / 3, 1 / 3, 1 / 3], [1, 0, 0], [0, 1, 0], [0, 0, 1], [.5, .5, 0], [0, .5, .5], [.5, 0, .5]]
    : [[1 / 3, 1 / 3, 1 / 3], [1, 0, 0], [0, 1, 0], [0, 0, 1], [.5, .5, 0], [0, .5, .5], [.5, 0, .5], [.6, .2, .2], [.2, .6, .2], [.2, .2, .6], [.75, .125, .125], [.125, .75, .125], [.125, .125, .75]];
  const triangles = Math.floor(soup.length / 9);
  const out = new Float32Array(triangles * weights.length * 3);
  let o = 0;
  for (let t = 0; t < triangles; t += 1) {
    const b = t * 9;
    for (const [a, c, d] of weights) {
      out[o++] = a * soup[b] + c * soup[b + 3] + d * soup[b + 6];
      out[o++] = a * soup[b + 1] + c * soup[b + 4] + d * soup[b + 7];
      out[o++] = a * soup[b + 2] + c * soup[b + 5] + d * soup[b + 8];
    }
  }
  return out;
}

/** Share of `points` that have some `reference` point within `radius`. */
export function shareNear(points: Float32Array, reference: Float32Array, radius: number): number {
  const count = Math.floor(points.length / 3);
  if (count === 0) return 0;
  const grid = new Map<string, number[]>();
  for (let i = 0; i < reference.length; i += 3) {
    const key = `${Math.floor(reference[i] / radius)},${Math.floor(reference[i + 1] / radius)},${Math.floor(reference[i + 2] / radius)}`;
    const list = grid.get(key);
    if (list) list.push(i); else grid.set(key, [i]);
  }
  let near = 0;
  for (let i = 0; i < count; i += 1) {
    const x = points[i * 3]; const y = points[i * 3 + 1]; const z = points[i * 3 + 2];
    const gx = Math.floor(x / radius); const gy = Math.floor(y / radius); const gz = Math.floor(z / radius);
    let hit = false;
    for (let dx = -1; dx <= 1 && !hit; dx += 1) for (let dy = -1; dy <= 1 && !hit; dy += 1) for (let dz = -1; dz <= 1 && !hit; dz += 1) {
      const list = grid.get(`${gx + dx},${gy + dy},${gz + dz}`);
      if (list === undefined) continue;
      for (const r of list) if (Math.hypot(reference[r] - x, reference[r + 1] - y, reference[r + 2] - z) <= radius) { hit = true; break; }
    }
    if (hit) near += 1;
  }
  return near / count;
}

export interface SurfaceQuality {
  /** Share of the mesh (sampled) that lies within `tolerance` of the true surface: how much of what is drawn is real. */
  precision: number;
  /** Share of the true surface (sampled) that has mesh within `tolerance`: how much of what exists is drawn. */
  recall: number;
}

/** `truth` is the true surface sampled densely (denser than the tolerance). */
export function surfaceQuality(soup: Float32Array, truth: Float32Array, tolerance: number): SurfaceQuality {
  const samples = sampleMesh(soup, 'dense');
  return { precision: shareNear(samples, truth, tolerance), recall: shareNear(truth, samples, tolerance) };
}

/** Samples of the mesh that are farther than `distance` from every measured point: surface nothing measured supports. */
export function unsupportedShare(soup: Float32Array, measured: Float32Array, distance: number): number {
  const samples = sampleMesh(soup, 'light');
  return 1 - shareNear(samples, measured, distance);
}
