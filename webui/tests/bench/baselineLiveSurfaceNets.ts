/*
 * A connected translucent skin over the measured surface, cheap enough to rebuild every couple of seconds on a phone (naive surface nets).
 *
 * The points are binned into a lattice of cells `cell` metres apart; a lattice point is "inside" when it holds a few points (or one with a neighbour).
 * Wherever an inside and an outside lattice point are neighbours the skin crosses between them. One vertex sits in every lattice cube the skin passes
 * through (the mean of its crossings), and every crossing becomes a quad of the four cubes around it, split into two triangles. The result is one
 * connected, non-overlapping triangle mesh that hugs the measured points: no per-point patches, no islands from a neighbour search, no component gating.
 * A thin real surface (a wall, a floor) comes out as a thin shell of two skins; both are kept, they are a cell apart and translucent.
 */

export interface SurfaceNetsInput {
  /** Project frame (Z up), metres; three floats per point. */
  positions: Float32Array;
  pointCount: number;
  /** Lattice spacing in metres. */
  cellMeters: number;
}

export interface SurfaceNetsResult {
  /** Triangle soup in the project frame, 9 floats per triangle. */
  triangles: Float32Array;
  triangleCount: number;
  occupiedCells: number;
}

const DIM = 4096;
const key = (x: number, y: number, z: number): number => (x * DIM + y) * DIM + z;
const AXES: ReadonlyArray<readonly [number, number, number]> = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];

/** The two axes other than `axis`, in cyclic order, so the four cubes around an edge come out in a consistent winding. */
const OTHER: ReadonlyArray<readonly [number, number]> = [[1, 2], [2, 0], [0, 1]];

export function buildSurfaceNets(input: SurfaceNetsInput): SurfaceNetsResult {
  const { positions, pointCount, cellMeters } = input;
  const empty: SurfaceNetsResult = { triangles: new Float32Array(0), triangleCount: 0, occupiedCells: 0 };
  if (!(cellMeters > 0) || pointCount < 1) return empty;
  let minX = Infinity; let minY = Infinity; let minZ = Infinity;
  for (let i = 0; i < pointCount; i += 1) {
    const x = positions[i * 3]; const y = positions[i * 3 + 1]; const z = positions[i * 3 + 2];
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    if (x < minX) minX = x; if (y < minY) minY = y; if (z < minZ) minZ = z;
  }
  if (!Number.isFinite(minX)) return empty;
  /* lattice coordinates start at 2 so that a neighbour of the lowest point is still a valid, non-negative lattice point */
  const ox = minX - 2 * cellMeters; const oy = minY - 2 * cellMeters; const oz = minZ - 2 * cellMeters;

  const counts = new Map<number, number>();
  for (let i = 0; i < pointCount; i += 1) {
    const x = positions[i * 3]; const y = positions[i * 3 + 1]; const z = positions[i * 3 + 2];
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    const ix = Math.round((x - ox) / cellMeters); const iy = Math.round((y - oy) / cellMeters); const iz = Math.round((z - oz) / cellMeters);
    if (ix < 1 || iy < 1 || iz < 1 || ix >= DIM - 2 || iy >= DIM - 2 || iz >= DIM - 2) continue;
    const k = key(ix, iy, iz);
    counts.set(k, (counts.get(k) ?? 0) + 1);
  }
  /* inside = two or more points, or one point that has an inside-or-single neighbour (a lone point is noise, a floater) */
  const occupied = new Set<number>();
  const decode = (k: number): [number, number, number] => [Math.floor(k / (DIM * DIM)), Math.floor(k / DIM) % DIM, k % DIM];
  for (const [k, n] of counts) {
    if (n >= 2) { occupied.add(k); continue; }
    const [x, y, z] = decode(k);
    let supported = false;
    for (let dx = -1; dx <= 1 && !supported; dx += 1) for (let dy = -1; dy <= 1 && !supported; dy += 1) for (let dz = -1; dz <= 1 && !supported; dz += 1) {
      if ((dx !== 0 || dy !== 0 || dz !== 0) && counts.has(key(x + dx, y + dy, z + dz))) supported = true;
    }
    if (supported) occupied.add(k);
  }
  if (occupied.size === 0) return empty;

  /* crossing edges: an inside lattice point next to an outside one (an edge has exactly one inside end, so each is found once) */
  const sums = new Map<number, [number, number, number, number]>();       /* cube (by its minimum corner) -> sum of crossing midpoints + count */
  const edges: Array<[number, number, number, number]> = [];
  for (const k of occupied) {
    const [x, y, z] = decode(k);
    for (let axis = 0; axis < 3; axis += 1) for (const sign of [-1, 1]) {
      const a = AXES[axis];
      const nx = x + sign * a[0]; const ny = y + sign * a[1]; const nz = z + sign * a[2];
      if (occupied.has(key(nx, ny, nz))) continue;
      const lx = sign < 0 ? nx : x; const ly = sign < 0 ? ny : y; const lz = sign < 0 ? nz : z;
      edges.push([lx, ly, lz, axis]);
      const mx = lx + a[0] / 2; const my = ly + a[1] / 2; const mz = lz + a[2] / 2;
      const [b, c] = OTHER[axis];
      for (let u = 0; u < 2; u += 1) for (let v = 0; v < 2; v += 1) {
        const q: [number, number, number] = [lx, ly, lz];
        q[b] -= u; q[c] -= v;
        const cubeKey = key(q[0], q[1], q[2]);
        const sum = sums.get(cubeKey);
        if (sum === undefined) sums.set(cubeKey, [mx, my, mz, 1]); else { sum[0] += mx; sum[1] += my; sum[2] += mz; sum[3] += 1; }
      }
    }
  }
  const cubeIndex = new Map<number, number>();
  const vertices: number[] = [];
  for (const [cubeKey, sum] of sums) {
    cubeIndex.set(cubeKey, vertices.length / 3);
    vertices.push(ox + (sum[0] / sum[3]) * cellMeters, oy + (sum[1] / sum[3]) * cellMeters, oz + (sum[2] / sum[3]) * cellMeters);
  }
  const triangles = new Float32Array(edges.length * 18);
  let count = 0;
  for (const [lx, ly, lz, axis] of edges) {
    const [b, c] = OTHER[axis];
    const corner = (u: number, v: number): number => {
      const q: [number, number, number] = [lx, ly, lz];
      q[b] -= u; q[c] -= v;
      return cubeIndex.get(key(q[0], q[1], q[2]))! * 3;
    };
    const quad = [corner(0, 0), corner(1, 0), corner(1, 1), corner(0, 1)];
    for (const tri of [[0, 1, 2], [0, 2, 3]]) {
      for (let k = 0; k < 3; k += 1) {
        const o = quad[tri[k]];
        triangles.set([vertices[o], vertices[o + 1], vertices[o + 2]], count * 9 + k * 3);
      }
      count += 1;
    }
  }
  return { triangles: triangles.slice(0, count * 9), triangleCount: count, occupiedCells: occupied.size };
}
