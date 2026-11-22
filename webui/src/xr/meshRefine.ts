/*
 * Post-processing of the finished scan mesh (phone run 2026-11-22: "the mesh is ragged"): small holes are closed and the surface is smoothed a little.
 * It only changes how the triangle soup LOOKS. It never touches the measured points, the occupancy or any saved artifact, and it is deliberately timid:
 *   - a hole is closed only when it is small (few edges, short perimeter, short edges), nearly flat, and the surface around it is substantial;
 *     the open edge of a scan, a doorway or a window is far larger than that and stays open;
 *   - smoothing moves a vertex at most a fraction of a voxel, never moves a vertex on an open boundary, and leaves non-manifold spots alone.
 */

export interface MeshRefineOptions {
  /** Edges of the longest loop that may be closed. */
  maxHoleEdges: number;
  maxHolePerimeterMeters: number;
  maxHoleEdgeMeters: number;
  /** Every loop vertex must lie this close to the loop's best-fit plane. */
  maxHoleFlatnessMeters: number;
  /** The surface around a hole must have at least this many triangles per hole edge. */
  minSurroundingTrianglesPerHoleEdge: number;
  smoothingIterations: number;
  smoothingStrength: number;
  /** Largest distance a smoothed vertex may end from where it started, in metres. */
  maxSmoothingDisplacementMeters: number;
  /** Vertices closer than this are one vertex when the soup is welded. */
  weldMeters: number;
}

export interface MeshRefineResult {
  trianglePositions: Float32Array;
  triangleCount: number;
  holesFilled: number;
  holeTrianglesAdded: number;
  openLoopsLeft: number;
  /** Why the loops that stayed open were not closed: a count per reason. */
  openReasons: Record<'pinched' | 'tooLarge' | 'notFlat' | 'smallSurface' | 'nonManifold', number>;
  smoothedVertices: number;
  maxDisplacementMeters: number;
}

export function defaultMeshRefineOptions(voxelSizeMeters: number): MeshRefineOptions {
  const voxel = Math.min(0.12, Math.max(0.02, Math.abs(voxelSizeMeters) || 0.05));
  return {
    maxHoleEdges: 12,
    maxHolePerimeterMeters: Math.max(0.30, voxel * 10),
    maxHoleEdgeMeters: Math.max(0.12, voxel * 2.5),
    maxHoleFlatnessMeters: Math.max(0.02, voxel * 0.45),
    minSurroundingTrianglesPerHoleEdge: 8,
    smoothingIterations: 2,
    smoothingStrength: 0.5,
    maxSmoothingDisplacementMeters: voxel * 0.25,
    weldMeters: Math.max(0.002, voxel * 0.10),
  };
}

type Vec = [number, number, number];

interface Welded {
  vertices: Vec[];
  triangles: number[][];
}

function weld(soup: Float32Array, tolerance: number): Welded {
  const ids = new Map<string, number>();
  const sums: Vec[] = []; const counts: number[] = [];
  const vertices: Vec[] = [];
  const triangles: number[][] = [];
  const lookup = (o: number): number => {
    const x = soup[o]; const y = soup[o + 1]; const z = soup[o + 2];
    const key = `${Math.round(x / tolerance)},${Math.round(y / tolerance)},${Math.round(z / tolerance)}`;
    let id = ids.get(key);
    if (id === undefined) { id = sums.length; ids.set(key, id); sums.push([0, 0, 0]); counts.push(0); }
    sums[id][0] += x; sums[id][1] += y; sums[id][2] += z; counts[id] += 1;
    return id;
  };
  for (let t = 0; t + 8 < soup.length; t += 9) {
    const a = lookup(t); const b = lookup(t + 3); const c = lookup(t + 6);
    if (a === b || b === c || a === c) continue;
    triangles.push([a, b, c]);
  }
  for (let i = 0; i < sums.length; i += 1) vertices.push([sums[i][0] / counts[i], sums[i][1] / counts[i], sums[i][2] / counts[i]]);
  return { vertices, triangles };
}

const edgeKey = (a: number, b: number): string => (a < b ? `${a}_${b}` : `${b}_${a}`);

function sub(a: Vec, b: Vec): Vec { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function cross(a: Vec, b: Vec): Vec { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function len(a: Vec): number { return Math.hypot(a[0], a[1], a[2]); }

/** Closes the small, flat, well-surrounded holes of a welded mesh; returns how many were closed and how many open loops remain. */
function fillHoles(mesh: Welded, options: MeshRefineOptions): { holes: number; added: number; open: number; reasons: MeshRefineResult['openReasons'] } {
  const reasons: MeshRefineResult['openReasons'] = { pinched: 0, tooLarge: 0, notFlat: 0, smallSurface: 0, nonManifold: 0 };
  const { vertices, triangles } = mesh;
  const counts = new Map<string, number>();
  for (const t of triangles) for (let e = 0; e < 3; e += 1) { const key = edgeKey(t[e], t[(e + 1) % 3]); counts.set(key, (counts.get(key) ?? 0) + 1); }
  const outgoing = new Map<number, number[]>();
  const nonManifoldVertex = new Set<number>();
  for (const t of triangles) for (let e = 0; e < 3; e += 1) {
    const a = t[e]; const b = t[(e + 1) % 3];
    const n = counts.get(edgeKey(a, b)) ?? 0;
    if (n > 2) { nonManifoldVertex.add(a); nonManifoldVertex.add(b); }
    if (n === 1) { const list = outgoing.get(a); if (list) list.push(b); else outgoing.set(a, [b]); }
  }
  /* component sizes (by shared vertex) to refuse to cap a tiny isolated patch */
  const parent = vertices.map((_, index) => index);
  const find = (a: number): number => { while (parent[a] !== a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
  for (const t of triangles) { parent[find(t[0])] = find(t[1]); parent[find(t[1])] = find(t[2]); }
  const componentTriangles = new Map<number, number>();
  for (const t of triangles) { const root = find(t[0]); componentTriangles.set(root, (componentTriangles.get(root) ?? 0) + 1); }

  const used = new Set<string>();
  let holes = 0; let added = 0; let open = 0;
  for (const [start, firsts] of outgoing) {
    for (const first of firsts) {
      const startKey = `${start}>${first}`;
      if (used.has(startKey)) continue;
      const loop: number[] = [start];
      let previous = start; let current = first; let closed = false; let simple = true;
      used.add(startKey);
      for (let guard = 0; guard <= options.maxHoleEdges + 1; guard += 1) {
        if (current === start) { closed = true; break; }
        loop.push(current);
        const next = outgoing.get(current);
        if (next === undefined || next.length !== 1) { simple = false; break; }       /* a pinched boundary vertex: not a plain hole */
        used.add(`${current}>${next[0]}`);
        previous = current; current = next[0];
        if (loop.length > options.maxHoleEdges) { simple = false; break; }
      }
      void previous;
      if (!closed || !simple) { open += 1; if (loop.length > options.maxHoleEdges) reasons.tooLarge += 1; else reasons.pinched += 1; continue; }
      if (loop.length < 3 || loop.some((v) => nonManifoldVertex.has(v))) { open += 1; reasons.nonManifold += 1; continue; }
      let perimeter = 0; let longest = 0;
      for (let i = 0; i < loop.length; i += 1) { const d = len(sub(vertices[loop[(i + 1) % loop.length]], vertices[loop[i]])); perimeter += d; longest = Math.max(longest, d); }
      if (perimeter > options.maxHolePerimeterMeters || longest > options.maxHoleEdgeMeters) { open += 1; reasons.tooLarge += 1; continue; }
      const centroid: Vec = [0, 0, 0];
      for (const v of loop) { centroid[0] += vertices[v][0] / loop.length; centroid[1] += vertices[v][1] / loop.length; centroid[2] += vertices[v][2] / loop.length; }
      const normal: Vec = [0, 0, 0];                                                  /* Newell normal of the loop */
      for (let i = 0; i < loop.length; i += 1) {
        const p = vertices[loop[i]]; const q = vertices[loop[(i + 1) % loop.length]];
        normal[0] += (p[1] - q[1]) * (p[2] + q[2]); normal[1] += (p[2] - q[2]) * (p[0] + q[0]); normal[2] += (p[0] - q[0]) * (p[1] + q[1]);
      }
      const normalLength = len(normal);
      if (normalLength < 1e-9) { open += 1; reasons.notFlat += 1; continue; }
      const unit: Vec = [normal[0] / normalLength, normal[1] / normalLength, normal[2] / normalLength];
      const flat = loop.every((v) => Math.abs((vertices[v][0] - centroid[0]) * unit[0] + (vertices[v][1] - centroid[1]) * unit[1] + (vertices[v][2] - centroid[2]) * unit[2]) <= options.maxHoleFlatnessMeters);
      if (!flat) { open += 1; reasons.notFlat += 1; continue; }
      if ((componentTriangles.get(find(loop[0])) ?? 0) < options.minSurroundingTrianglesPerHoleEdge * loop.length) { open += 1; reasons.smallSurface += 1; continue; }
      const centre = vertices.length;
      vertices.push(centroid);
      parent.push(centre); parent[centre] = find(loop[0]);
      /* The boundary half-edges run with the existing faces on their left, so the cap uses each edge reversed to stay consistently oriented. */
      for (let i = 0; i < loop.length; i += 1) triangles.push([centre, loop[(i + 1) % loop.length], loop[i]]);
      holes += 1; added += loop.length;
    }
  }
  return { holes, added, open, reasons };
}

function smooth(mesh: Welded, options: MeshRefineOptions): { moved: number; maxDisplacement: number } {
  const { vertices, triangles } = mesh;
  if (options.smoothingIterations <= 0 || options.smoothingStrength <= 0) return { moved: 0, maxDisplacement: 0 };
  const counts = new Map<string, number>();
  for (const t of triangles) for (let e = 0; e < 3; e += 1) { const key = edgeKey(t[e], t[(e + 1) % 3]); counts.set(key, (counts.get(key) ?? 0) + 1); }
  const neighbours: Array<Set<number>> = vertices.map(() => new Set<number>());
  const frozen = new Set<number>();
  for (const t of triangles) for (let e = 0; e < 3; e += 1) {
    const a = t[e]; const b = t[(e + 1) % 3];
    neighbours[a].add(b); neighbours[b].add(a);
    const n = counts.get(edgeKey(a, b)) ?? 0;
    if (n !== 2) { frozen.add(a); frozen.add(b); }                                   /* open boundary or non-manifold: keep as measured */
  }
  const original = vertices.map((v) => [...v] as Vec);
  let current = vertices.map((v) => [...v] as Vec);
  for (let iteration = 0; iteration < options.smoothingIterations; iteration += 1) {
    const next = current.map((v) => [...v] as Vec);
    for (let i = 0; i < current.length; i += 1) {
      if (frozen.has(i) || neighbours[i].size < 3) continue;
      const average: Vec = [0, 0, 0];
      for (const n of neighbours[i]) { average[0] += current[n][0]; average[1] += current[n][1]; average[2] += current[n][2]; }
      const k = neighbours[i].size;
      const target: Vec = [
        current[i][0] + options.smoothingStrength * (average[0] / k - current[i][0]),
        current[i][1] + options.smoothingStrength * (average[1] / k - current[i][1]),
        current[i][2] + options.smoothingStrength * (average[2] / k - current[i][2]),
      ];
      const offset = sub(target, original[i]);
      const distance = len(offset);
      const scale = distance > options.maxSmoothingDisplacementMeters ? options.maxSmoothingDisplacementMeters / distance : 1;
      next[i] = [original[i][0] + offset[0] * scale, original[i][1] + offset[1] * scale, original[i][2] + offset[2] * scale];
    }
    current = next;
  }
  let moved = 0; let maxDisplacement = 0;
  for (let i = 0; i < vertices.length; i += 1) {
    const d = len(sub(current[i], original[i]));
    if (d > 1e-7) moved += 1;
    maxDisplacement = Math.max(maxDisplacement, d);
    vertices[i] = current[i];
  }
  return { moved, maxDisplacement };
}

/** Closes small flat holes, then smooths a little. Output is a triangle soup like the input; an empty or unusable input is returned unchanged. */
export function refineScanMesh(soup: Float32Array, voxelSizeMeters: number, overrides: Partial<MeshRefineOptions> = {}): MeshRefineResult {
  const options = { ...defaultMeshRefineOptions(voxelSizeMeters), ...overrides };
  const unchanged = (): MeshRefineResult => ({ trianglePositions: soup, triangleCount: Math.floor(soup.length / 9), holesFilled: 0, holeTrianglesAdded: 0, openLoopsLeft: 0, openReasons: { pinched: 0, tooLarge: 0, notFlat: 0, smallSurface: 0, nonManifold: 0 }, smoothedVertices: 0, maxDisplacementMeters: 0 });
  if (soup.length < 27 || !Array.from(soup.subarray(0, Math.min(soup.length, 90))).every(Number.isFinite)) return unchanged();
  const mesh = weld(soup, options.weldMeters);
  if (mesh.triangles.length === 0) return unchanged();
  const holes = fillHoles(mesh, options);
  const smoothing = smooth(mesh, options);
  const out = new Float32Array(mesh.triangles.length * 9);
  mesh.triangles.forEach((t, index) => {
    for (let corner = 0; corner < 3; corner += 1) {
      const v = mesh.vertices[t[corner]];
      out[index * 9 + corner * 3] = v[0]; out[index * 9 + corner * 3 + 1] = v[1]; out[index * 9 + corner * 3 + 2] = v[2];
    }
  });
  return {
    trianglePositions: out, triangleCount: mesh.triangles.length, holesFilled: holes.holes, holeTrianglesAdded: holes.added,
    openLoopsLeft: holes.open, openReasons: holes.reasons, smoothedVertices: smoothing.moved, maxDisplacementMeters: smoothing.maxDisplacement,
  };
}
