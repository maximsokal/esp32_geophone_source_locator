import { describe, expect, it } from 'vitest';

import { buildSurfaceNets } from '../../src/xr/liveSurfaceNets';

const CELL = 0.08;

function planePoints(side: number, spacing: number, z: number, x0 = 0, y0 = 0): number[] {
  const out: number[] = [];
  for (let i = 0; i < side; i += 1) for (let j = 0; j < side; j += 1) out.push(x0 + i * spacing, y0 + j * spacing, z);
  return out;
}
const run = (points: number[], cell = CELL) => buildSurfaceNets({ positions: Float32Array.from(points), pointCount: points.length / 3, cellMeters: cell });

/** Welds the soup by exact corner position; returns per-triangle vertex ids and edge use counts. */
function topology(triangles: Float32Array) {
  const ids = new Map<string, number>();
  const id = (o: number): number => { const k = `${triangles[o].toFixed(5)},${triangles[o + 1].toFixed(5)},${triangles[o + 2].toFixed(5)}`; let v = ids.get(k); if (v === undefined) { v = ids.size; ids.set(k, v); } return v; };
  const edgeUse = new Map<string, number>();
  const tris: number[][] = [];
  for (let t = 0; t + 8 < triangles.length; t += 9) {
    const v = [id(t), id(t + 3), id(t + 6)];
    tris.push(v);
    for (let e = 0; e < 3; e += 1) { const a = v[e]; const b = v[(e + 1) % 3]; const k = a < b ? `${a}_${b}` : `${b}_${a}`; edgeUse.set(k, (edgeUse.get(k) ?? 0) + 1); }
  }
  const parent = Array.from({ length: ids.size }, (_, i) => i);
  const find = (a: number): number => { while (parent[a] !== a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
  for (const [a, b, c] of tris) { parent[find(a)] = find(b); parent[find(b)] = find(c); }
  const components = new Set(tris.map((t) => find(t[0]))).size;
  return { vertices: ids.size, edgeUse, components, tris };
}

function maxDistanceToMesh(points: number[], triangles: Float32Array): number {
  let worst = 0;
  const verts: number[][] = [];
  for (let i = 0; i + 2 < triangles.length; i += 3) verts.push([triangles[i], triangles[i + 1], triangles[i + 2]]);
  for (let p = 0; p < points.length; p += 3) {
    let best = Infinity;
    for (const v of verts) best = Math.min(best, Math.hypot(v[0] - points[p], v[1] - points[p + 1], v[2] - points[p + 2]));
    worst = Math.max(worst, best);
  }
  return worst;
}

describe('the live surface skin (surface nets over the measured points)', () => {
  it('turns a measured plane into ONE connected skin that stays within a cell of the plane and of every point', () => {
    const points = planePoints(30, 0.04, 0.8);
    const result = run(points);
    expect(result.triangleCount).toBeGreaterThan(200);
    const { components } = topology(result.triangles);
    expect(components, 'one connected surface, not islands').toBe(1);
    for (let i = 2; i < result.triangles.length; i += 3) expect(Math.abs(result.triangles[i] - 0.8)).toBeLessThanOrEqual(CELL);
    expect(maxDistanceToMesh(points, result.triangles), 'the skin hugs every measured point').toBeLessThanOrEqual(CELL * 1.2);
  });

  it('does not bridge a gap between two separate surfaces, and does not invent surface where nothing was measured', () => {
    const points = [...planePoints(20, 0.04, 0.5), ...planePoints(20, 0.04, 0.5, 3, 0)];             /* two patches 2 m apart */
    const result = run(points);
    expect(topology(result.triangles).components).toBe(2);
    for (let i = 0; i < result.triangles.length; i += 3) {
      const x = result.triangles[i];
      expect(x < 0.8 + CELL || x > 3 - CELL, `no triangle in the empty space between the patches (x=${x})`).toBe(true);
    }
  });

  it('closes a solid blob into a watertight mesh: every edge belongs to exactly two triangles', () => {
    const points: number[] = [];
    for (let x = -0.3; x <= 0.3; x += 0.02) for (let y = -0.3; y <= 0.3; y += 0.02) for (let z = -0.3; z <= 0.3; z += 0.02) if (Math.hypot(x, y, z) < 0.28) points.push(x, y, z);
    const result = run(points);
    expect(result.triangleCount).toBeGreaterThan(100);
    const { edgeUse, components } = topology(result.triangles);
    expect(components).toBe(1);
    for (const [edge, uses] of edgeUse) expect(uses, `edge ${edge}`).toBe(2);
  });

  it('ignores a lone noisy point but keeps two points in a cell and a point with a neighbour', () => {
    expect(run([1, 1, 1]).triangleCount).toBe(0);
    expect(run([1, 1, 1, 5, 5, 5]).triangleCount, 'two far apart lone points').toBe(0);
    expect(run([1, 1, 1, 1.001, 1, 1]).triangleCount, 'two points in one cell').toBeGreaterThan(0);
    expect(run([1, 1, 1, 1.07, 1, 1]).triangleCount, 'a point and its neighbour').toBeGreaterThan(0);
  });

  it('skips non-finite points, handles empty input and an unusable cell size, and is deterministic', () => {
    const points = planePoints(10, 0.04, 0.3);
    const damaged = [...points, Number.NaN, 0, 0, 0, Infinity, 0];
    expect(Array.from(run(damaged).triangles)).toEqual(Array.from(run(points).triangles));
    expect(run([]).triangleCount).toBe(0);
    expect(run(points, 0).triangleCount).toBe(0);
    expect(run(points, Number.NaN).triangleCount).toBe(0);
    expect(Array.from(run(points).triangles)).toEqual(Array.from(run(points).triangles));
    expect(Array.from(run(points).triangles).every(Number.isFinite)).toBe(true);
  });

  it('a larger cell gives fewer triangles over the same surface', () => {
    const points = planePoints(40, 0.04, 0.8);
    expect(run(points, 0.12).triangleCount).toBeLessThan(run(points, 0.06).triangleCount);
  });

  it('keeps every triangle small: no sliver stretches across the scene', () => {
    const result = run(planePoints(40, 0.04, 0.8));
    for (let t = 0; t + 8 < result.triangles.length; t += 9) {
      for (let a = 0; a < 3; a += 1) {
        const b = (a + 1) % 3;
        const length = Math.hypot(result.triangles[t + a * 3] - result.triangles[t + b * 3], result.triangles[t + a * 3 + 1] - result.triangles[t + b * 3 + 1], result.triangles[t + a * 3 + 2] - result.triangles[t + b * 3 + 2]);
        expect(length).toBeLessThanOrEqual(CELL * 2.5);
      }
    }
  });
});
