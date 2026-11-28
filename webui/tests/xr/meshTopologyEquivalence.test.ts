// Exact results of the topology passes where an implementation detail could change them: the order of the
// candidates (it decides what a budget cuts off), edge identity and the edge analysis after a repair.
// The golden values were produced by the string-key implementation (commit 17e33509) that these passes replaced.
import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { createMeshVertexIds, meshEdgeKey } from '../../src/xr/meshKeys';
import { diagnoseMeshNormalIntrusions } from '../../src/xr/meshNormalIntrusion';
import { diagnoseMeshSelfIntersections } from '../../src/xr/meshSelfIntersection';
import { sanitizeMeshTopology } from '../../src/xr/meshTopologySanity';

function random(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 4294967296; };
}

/** `count` triangles (less the needles) with shared corners and folds in a box of side `side`; the first `huge` of them span it. */
function soup(seed: number, count: number, side: number, huge: number): Float32Array {
  const next = random(seed);
  const out: number[] = [];
  const corners = Array.from({ length: Math.ceil(count * 0.9) }, () => [next() * side, next() * side, Math.round(next() * 4) * 0.03 + next() * 0.01]);
  for (let t = 0; t < count; t += 1) {
    const a = corners[Math.floor(next() * corners.length)];
    const near = (k: number): number[] => (k === 0 ? a : t < huge
      ? [next() * side * 3, next() * side * 3, next()]
      : next() < 0.5
        ? corners[Math.floor(next() * corners.length)].map((v, i) => a[i] + (v - a[i]) * 0.08)
        : [a[0] + next() * 0.1 - 0.05, a[1] + next() * 0.1 - 0.05, a[2] + next() * 0.06 - 0.03]);
    const face = [near(0), near(1), near(2)];
    // Needles with welded corners are left out: the replaced implementation crashed on them (meshSelfIntersection.test.ts covers them).
    const shortest = Math.min(...face.map((p, i) => Math.hypot(...p.map((v, axis) => v - face[(i + 1) % 3][axis]))));
    if (shortest > 2e-3) out.push(...face.flat());
  }
  return Float32Array.from(out);
}

/** A flat patch with a thin ledge, a second shell above part of it and fins that make three-face edges. */
function patchWithFins(cells: number, spacing: number): Float32Array {
  const out: number[] = [];
  const quad = (x: number, y: number, z: number): void => {
    const x1 = (x + 1) * spacing, y1 = (y + 1) * spacing, x0 = x * spacing, y0 = y * spacing;
    out.push(x0, y0, z, x1, y0, z, x1, y1, z, x0, y0, z, x1, y1, z, x0, y1, z);
  };
  for (let y = 0; y < cells; y += 1) for (let x = 0; x < cells; x += 1) quad(x, y, 0);
  for (let y = 0; y < 3; y += 1) for (let x = 0; x < 3; x += 1) quad(x, y, 0.045);
  // the ledge hangs on one whole edge of the patch and is 6 mm wide after that
  for (let x = cells; x < cells + 5; x += 1) {
    const x0 = x * spacing, x1 = x0 + spacing, height = x === cells ? spacing : 0.006;
    out.push(x0, 0, 0, x1, 0, 0, x1, 0.006, 0, x0, 0, 0, x1, 0.006, 0, x0, height, 0);
  }
  for (let x = 2; x < cells - 2; x += 3) {
    const x0 = x * spacing, x1 = x0 + spacing, y = 5 * spacing;
    out.push(x0, y, 0, x1, y, 0, (x0 + x1) / 2, y + 0.002, 0.012);
  }
  return Float32Array.from(out);
}

/** triangles with the string vertex keys of a hand-built mesh */
function withStringKeys(positions: Float32Array) {
  return Array.from({ length: positions.length / 9 }, (_, t) => {
    const vertices = [0, 1, 2].map((c) => [positions[t * 9 + c * 3], positions[t * 9 + c * 3 + 1], positions[t * 9 + c * 3 + 2]] as [number, number, number]) as [[number, number, number], [number, number, number], [number, number, number]];
    return { vertices, keys: vertices.map((p) => p.map((v) => Math.round(v / 8e-4)).join(':')) as [string, string, string] };
  });
}

/** everything the pass returns except its timings; typed arrays as hashes */
function digest(value: unknown): string {
  const canonical = (item: unknown): unknown => {
    if (ArrayBuffer.isView(item)) return createHash('sha1').update(new Uint8Array(item.buffer, item.byteOffset, item.byteLength)).digest('hex').slice(0, 16);
    if (Array.isArray(item)) return item.map(canonical);
    if (item !== null && typeof item === 'object') return Object.fromEntries(Object.entries(item).filter(([key]) => !/Ms$/.test(key)).map(([key, v]) => [key, canonical(v)]));
    return item;
  };
  return createHash('sha256').update(JSON.stringify(canonical(value))).digest('hex').slice(0, 20);
}

const CASES = [
  { name: 'dense overlapping soup (both budgets cut)', positions: () => soup(2, 2600, 0.16, 0), voxel: 0.048 },
  { name: 'soup with many triangles too wide for the grid', positions: () => soup(4, 900, 1.2, 200), voxel: 0.03 },
  { name: 'soup far from the origin', positions: () => soup(6, 800, 90, 3), voxel: 0.048 },
  { name: 'patch with three-face edges, a ledge and a second shell', positions: () => patchWithFins(16, 0.04), voxel: 0.048 },
];

const GOLDEN: Record<string, { intrusion: string; selfIntersection: string; sanitized: string }> = {
  'dense overlapping soup (both budgets cut)': { intrusion: 'b9ae4837e2240eed8687', selfIntersection: '4801d5d9ea61942fc031', sanitized: '8ccb6856fa2c3c4ca840' },
  'soup with many triangles too wide for the grid': { intrusion: '6cca9dcad5540434931c', selfIntersection: 'bb1bb2141955983b420e', sanitized: 'c33bd51656ca0c084e58' },
  'soup far from the origin': { intrusion: '534e18b2495e73e33c4c', selfIntersection: '00b20de03edde7458285', sanitized: '0210be22dc8d2b511562' },
  'patch with three-face edges, a ledge and a second shell': { intrusion: '66e0574da9646e529fd7', selfIntersection: '5966bde2d153a13082af', sanitized: 'af3e9346faa5595a4dde' },
};

describe('topology passes give exactly the results of the implementation they replaced', () => {
  it.each(CASES)('$name', ({ name, positions, voxel }) => {
    const mesh = positions();
    const triangles = withStringKeys(mesh);
    const components = Int32Array.from(triangles, (_, i) => i % 3);
    const actual = {
      intrusion: digest(diagnoseMeshNormalIntrusions(triangles, components, 3, voxel)),
      selfIntersection: digest(diagnoseMeshSelfIntersections(triangles, components, 3, voxel)),
      sanitized: digest(sanitizeMeshTopology(mesh, voxel)),
    };
    expect(actual).toEqual(GOLDEN[name]);
  });

  it('the cases reach what they are meant to reach', () => {
    const dense = withStringKeys(CASES[0].positions());
    const labels = new Int32Array(dense.length);
    expect(diagnoseMeshNormalIntrusions(dense, labels, 1, 0.048).truncated).toBe(true);
    expect(diagnoseMeshSelfIntersections(dense, labels, 1, 0.048).truncated).toBe(true);
    const wide = withStringKeys(CASES[1].positions());
    expect(diagnoseMeshNormalIntrusions(wide, new Int32Array(wide.length), 1, 0.03).oversizedTriangleCount).toBeGreaterThan(128);
    const patch = sanitizeMeshTopology(CASES[3].positions(), 0.048);
    expect(patch.nonManifoldRepairTriangleCount).toBeGreaterThan(0);
    expect(patch.retainedTriangleCount).toBeGreaterThan(400);
    expect(patch.subscaleBranchTriangleCount).toBeGreaterThan(0);
  });
});

describe('numeric mesh keys', () => {
  it.each([
    ['near the origin', 0],
    ['past the packed range, where a cell falls back to a string', 70_000],
    ['past 32 bits', 5_000_000_000],
  ])('welds equal cells and keeps neighbouring cells apart: %s', (_, cell) => {
    const q = 8e-4;
    const id = createMeshVertexIds(q);
    const at = (x: number, y: number, z: number): number => id((cell + x) * q, (cell + y) * q, (cell + z) * q);
    const first = at(0, 0, 0);
    expect(at(0.2, -0.2, 0.2)).toBe(first);
    const others = [at(1, 0, 0), at(0, 1, 0), at(0, 0, 1), at(0, 1, -131072), at(1, -131072, 0)];
    expect(new Set([first, ...others]).size).toBe(1 + others.length);
    expect(at(1, 0, 0)).toBe(others[0]);
  });

  it('gives one key per undirected edge, for ids and for strings', () => {
    const keys = new Set<string | number>();
    for (let a = 0; a < 40; a += 1) for (let b = a + 1; b < 40; b += 1) {
      expect(meshEdgeKey(b, a)).toBe(meshEdgeKey(a, b));
      keys.add(meshEdgeKey(a, b));
    }
    expect(keys.size).toBe((40 * 39) / 2);
    const top = 2 ** 26 - 1;
    expect(meshEdgeKey(top, top - 1)).not.toBe(meshEdgeKey(top - 2, top - 1));
    expect(meshEdgeKey('b', 'a')).toBe(meshEdgeKey('a', 'b'));
  });
});
