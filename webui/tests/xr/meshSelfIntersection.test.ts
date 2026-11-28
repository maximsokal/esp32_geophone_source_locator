import { Vector3 } from 'three';
import { ExtendedTriangle } from 'three-mesh-bvh';
import { describe, expect, it } from 'vitest';

import { sanitizeMeshTopology } from '../../src/xr/meshTopologySanity';
import {
  diagnoseMeshSelfIntersections,
  MAX_SELF_INTERSECTION_EXACT_TESTS,
  trianglesIntersect,
  type MeshIntersectionPoint,
  type MeshIntersectionTriangle,
} from '../../src/xr/meshSelfIntersection';

function triangle(
  id: string,
  a: MeshIntersectionPoint,
  b: MeshIntersectionPoint,
  c: MeshIntersectionPoint,
): MeshIntersectionTriangle {
  return { vertices: [a, b, c], keys: [`${id}:a`, `${id}:b`, `${id}:c`] };
}

function sharedEdgeFold(): readonly MeshIntersectionTriangle[] {
  return [
    {
      vertices: [[0, 0, 0], [0.12, 0, 0], [0, 0.12, 0]],
      keys: ['o', 'x', 'y'],
    },
    {
      vertices: [[0, 0, 0], [0, 0, 0.12], [0.12, 0, 0]],
      keys: ['o', 'z', 'x'],
    },
  ];
}

describe('bounded mesh self-intersection diagnostic', () => {
  it('detects a genuine non-adjacent 3-D triangle crossing', () => {
    const horizontal = triangle('h', [-0.1, -0.1, 0], [0.1, -0.1, 0], [0, 0.12, 0]);
    const vertical = triangle('v', [0, -0.05, -0.1], [0, 0.10, 0.1], [0, 0.12, -0.1]);
    expect(trianglesIntersect(horizontal, vertical, 1e-5)).toBe(true);
    const diagnostic = diagnoseMeshSelfIntersections(
      [horizontal, vertical],
      Int32Array.from([0, 1]),
      2,
      0.04,
    );
    expect(diagnostic.intersectingPairCount).toBe(1);
    expect(diagnostic.affectedTriangleCount).toBe(2);
    expect(diagnostic.componentStats[0].affectedTriangleRatio).toBe(1);
    expect(diagnostic.componentStats[1].affectedTriangleRatio).toBe(1);
  });

  it('does not call a legitimate adjacent 90-degree fold self-intersection noise', () => {
    const folded = sharedEdgeFold();
    const diagnostic = diagnoseMeshSelfIntersections(
      folded,
      Int32Array.from([0, 0]),
      1,
      0.04,
    );
    expect(diagnostic.exactTestedPairs, 'the neighbours are examined, not skipped').toBe(1);
    expect(diagnostic.intersectingPairCount).toBe(0);
    expect(diagnostic.componentStats[0].affectedTriangleRatio).toBe(0);
  });

  it('detects coplanar overlap but rejects separated coplanar triangles', () => {
    const base = triangle('base', [0, 0, 0], [0.12, 0, 0], [0, 0.12, 0]);
    const overlap = triangle('overlap', [0.02, 0.02, 0], [0.10, 0.02, 0], [0.02, 0.10, 0]);
    const separated = triangle('separated', [0.30, 0, 0], [0.42, 0, 0], [0.30, 0.12, 0]);
    expect(trianglesIntersect(base, overlap, 1e-5)).toBe(true);
    expect(trianglesIntersect(base, separated, 1e-5)).toBe(false);
  });

  /* Triangles over shared points: a vertex key is the point it indexes, so adjacency is exactly what a mesh would report. */
  const mesh = (points: Record<string, MeshIntersectionPoint>, faces: string[]): MeshIntersectionTriangle[] =>
    faces.map((face) => { const keys = face.split(' ') as [string, string, string]; return { keys, vertices: [points[keys[0]], points[keys[1]], points[keys[2]]] }; });
  const diagnose = (triangles: MeshIntersectionTriangle[]) => diagnoseMeshSelfIntersections(triangles, new Int32Array(triangles.length), 1, 0.04);
  const FLOOR: Record<string, MeshIntersectionPoint> = { o: [0, 0, 0], x: [0.12, 0, 0], y: [0, 0.12, 0] };

  const INJECTED: Array<[string, MeshIntersectionTriangle[]]> = [
    ['a triangle that PIERCES a neighbour it shares one vertex with', mesh({ ...FLOOR, p: [0.04, 0.04, 0.05], q: [0.04, 0.04, -0.05] }, ['o x y', 'o p q'])],
    ['a triangle FOLDED BACK onto the neighbour it shares an edge with', mesh({ ...FLOOR, w: [0.02, 0.06, 0] }, ['o x y', 'o x w'])],
    ['two coplanar triangles that share one vertex and OVERLAP', mesh({ ...FLOOR, r: [0.10, 0.02, 0], s: [0.02, 0.10, 0] }, ['o x y', 'o r s'])],
    ['the same triangle emitted TWICE', mesh(FLOOR, ['o x y', 'o x y'])],
    ['a HUGE triangle crossed by a tiny one', [triangle('big', [-3, -3, 0], [3, -3, 0], [0, 3, 0]), triangle('tiny', [0, 0, -0.02], [0.02, 0, 0.02], [-0.02, 0, 0.02])]],
  ];
  it.each(INJECTED)('finds %s', (_name, triangles) => {
    const diagnostic = diagnose(triangles);
    expect([diagnostic.intersectingPairCount, diagnostic.affectedTriangleCount, diagnostic.truncated, diagnostic.oversizedTriangleCount]).toEqual([1, 2, false, 0]);
  });

  it('finds every crossing of a HUGE triangle, whether the small triangle is taken before or after it', () => {
    /* The order of the pairs follows the geometry, not the input: with twelve small triangles across the big one, some come before it and some after. */
    const big = triangle('big', [-3, -3, 0], [3, -3, 0], [0, 3, 0]);
    const small = Array.from({ length: 12 }, (_, i) => { const x = -1.1 + i * 0.2; return triangle(`s${i}`, [x, -1, -0.02], [x + 0.02, -1, 0.02], [x - 0.02, -1, 0.02]); });
    const diagnostic = diagnose([big, ...small]);
    expect([diagnostic.intersectingPairCount, diagnostic.affectedTriangleCount, diagnostic.truncated]).toEqual([12, 13, false]);
  });

  it('reports NOTHING for clean meshes: a flat grid (every neighbour coplanar), a closed cube and a sharp fan', () => {
    const points: Record<string, MeshIntersectionPoint> = {}; const faces: string[] = [];
    for (let i = 0; i <= 12; i += 1) for (let j = 0; j <= 12; j += 1) points[`g${i}_${j}`] = [i * 0.03, j * 0.03, 0];
    for (let i = 0; i < 12; i += 1) for (let j = 0; j < 12; j += 1) faces.push(`g${i}_${j} g${i + 1}_${j} g${i + 1}_${j + 1}`, `g${i}_${j} g${i + 1}_${j + 1} g${i}_${j + 1}`);
    const cube: Record<string, MeshIntersectionPoint> = {};
    for (let c = 0; c < 8; c += 1) cube[`c${c}`] = [(c & 1) * 0.1 + 1, ((c >> 1) & 1) * 0.1, ((c >> 2) & 1) * 0.1];
    const cubeFaces = ['c0 c2 c3', 'c0 c3 c1', 'c4 c5 c7', 'c4 c7 c6', 'c0 c1 c5', 'c0 c5 c4', 'c2 c6 c7', 'c2 c7 c3', 'c0 c4 c6', 'c0 c6 c2', 'c1 c3 c7', 'c1 c7 c5'];
    const fan: Record<string, MeshIntersectionPoint> = { h: [2, 0, 0] }; const fanFaces: string[] = [];
    for (let k = 0; k <= 10; k += 1) fan[`f${k}`] = [2 + 0.1 * Math.cos(k * 0.5), 0.1 * Math.sin(k * 0.5), k % 2 === 0 ? 0.03 : -0.03];
    for (let k = 0; k < 10; k += 1) fanFaces.push(`h f${k} f${k + 1}`);
    const diagnostic = diagnose([...mesh(points, faces), ...mesh(cube, cubeFaces), ...mesh(fan, fanFaces)]);
    expect([diagnostic.intersectingPairCount, diagnostic.truncated]).toEqual([0, false]);
  });

  it('misses no pair: on a random soup it agrees with testing every pair, and with the triangle test of three-mesh-bvh', () => {
    let seed = 12345; const random = (): number => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed / 4294967296; };
    const triangles = Array.from({ length: 160 }, (_, i) => {
      const centre = [random() * 0.6, random() * 0.6, random() * 0.6];
      const corner = (): MeshIntersectionPoint => [centre[0] + random() * 0.2 - 0.1, centre[1] + random() * 0.2 - 0.1, centre[2] + random() * 0.2 - 0.1];
      return triangle(`r${i}`, corner(), corner(), corner());
    });
    const oracle = triangles.map((t) => new ExtendedTriangle(...(t.vertices.map((v) => new Vector3(...v)) as [Vector3, Vector3, Vector3])));
    let everyPair = 0; let reference = 0;
    for (let i = 0; i < triangles.length; i += 1) for (let j = i + 1; j < triangles.length; j += 1) {
      if (trianglesIntersect(triangles[i], triangles[j], 6e-4)) everyPair += 1;
      oracle[i].needsUpdate = true;
      if (oracle[i].intersectsTriangle(oracle[j])) reference += 1;
    }
    expect(everyPair, 'precondition: the soup does intersect').toBeGreaterThan(50);
    expect(diagnose(triangles).intersectingPairCount).toBe(everyPair);
    expect(Math.abs(everyPair - reference), `own test ${everyPair}, three-mesh-bvh ${reference}`).toBeLessThanOrEqual(Math.ceil(reference * 0.01));
  });

  it('hard-caps exact tests for a pathological overlapping soup', () => {
    const triangles: MeshIntersectionTriangle[] = [];
    for (let index = 0; index < 220; index += 1) {
      const angle = index * 0.017;
      const dx = Math.cos(angle) * 0.004;
      const dy = Math.sin(angle) * 0.004;
      triangles.push(triangle(
        `s${index}`,
        [-0.12 + dx, -0.10 + dy, 0],
        [0.12 + dx, -0.10 + dy, 0],
        [dx, 0.13 + dy, 0],
      ));
    }
    const diagnostic = diagnoseMeshSelfIntersections(
      triangles,
      new Int32Array(triangles.length),
      1,
      0.04,
    );
    expect(diagnostic.truncated).toBe(true);
    expect(diagnostic.exactTestedPairs).toBe(MAX_SELF_INTERSECTION_EXACT_TESTS);
    expect(diagnostic.intersectingPairCount).toBeGreaterThan(0);
    expect(diagnostic.affectedTriangleCount).toBeGreaterThan(0);
  });

  // Two corners of a needle closer than the weld tolerance share one vertex id. A neighbour then has fewer
  // "own" corners than the neighbour rules expect; this crashed the whole reconstruction.
  it.each([0, 0.31, 0.77, 1.13, 1.9, 2.6])('survives a needle with welded corners next to a crossing neighbour (shifted by %d m)', (shift) => {
    const at = (x: number, y: number, z: number): number[] => [shift + x, shift * 0.5 + y, z];
    const needle = [at(0, 0, 0), at(0.0002, 0, 0), at(0.03, 0.01, 0)];
    const blade = [at(0, 0, 0), at(0.02, 0, 0.02), at(0.02, 0, -0.02)];
    for (const faces of [[needle, blade], [blade, needle]]) {
      const positions = Float32Array.from(faces.flat(2));
      expect(() => sanitizeMeshTopology(positions, 0.048)).not.toThrow();
      expect(sanitizeMeshTopology(positions, 0.048).selfIntersectionPairCount).toBe(0);
    }
  });
});
