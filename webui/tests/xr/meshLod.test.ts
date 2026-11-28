// Stage 6: the drawn mesh is stored indexed without loss, and the copy offered to the scanner is simplified by
// meshoptimizer without moving borders, merging shells or leaving the tolerance.
import { BufferAttribute, BufferGeometry, Vector3 } from 'three';
import { MeshBVH } from 'three-mesh-bvh';
import { describe, expect, it } from 'vitest';
import { surfaceMeshGeometry } from '../../src/components/ScanPointCloudViewport';
import { indexTriangleSoup, type IndexedMesh } from '../../src/xr/meshIndex';
import { DISPLAY_LOD_TOLERANCE_VOXELS, simplifyMeshForDisplay, type SimplifierModule } from '../../src/xr/meshLod';

type Point = [number, number, number];
const VOXEL = 0.048;
const CELL = 0.04;

/** `columns` x `rows` quads placed by `at`, without those `skip` names */
function sheet(columns: number, rows: number, at: (u: number, v: number) => Point, skip: (u: number, v: number) => boolean = () => false): number[] {
  const out: number[] = [];
  for (let v = 0; v < rows; v += 1) for (let u = 0; u < columns; u += 1) {
    if (skip(u, v)) continue;
    const a = at(u, v), b = at(u + 1, v), c = at(u + 1, v + 1), d = at(u, v + 1);
    out.push(...a, ...b, ...c, ...a, ...c, ...d);
  }
  return out;
}
const flat = (z: number) => (u: number, v: number): Point => [u * CELL, v * CELL, z];

function expand(mesh: IndexedMesh): Float32Array {
  const soup = new Float32Array(mesh.indices.length * 3);
  mesh.indices.forEach((vertex, corner) => soup.set(mesh.positions.subarray(vertex * 3, vertex * 3 + 3), corner * 3));
  return soup;
}
const bits = (soup: Float32Array): Uint32Array => new Uint32Array(soup.buffer, soup.byteOffset, soup.length);
const corners = (soup: Float32Array): string[] => Array.from({ length: soup.length / 3 }, (_, i) => `${soup[i * 3]},${soup[i * 3 + 1]},${soup[i * 3 + 2]}`);
/** edges used by a number of triangles that `wanted` accepts, as pairs of corner names */
function edgesUsed(soup: Float32Array, wanted: (uses: number) => boolean): string[] {
  const names = corners(soup);
  const uses = new Map<string, number>();
  for (let t = 0; t < names.length; t += 3) for (let side = 0; side < 3; side += 1) {
    const key = [names[t + side], names[t + (side + 1) % 3]].sort().join(' ');
    uses.set(key, (uses.get(key) ?? 0) + 1);
  }
  return [...uses].filter(([, n]) => wanted(n)).map(([key]) => key).sort();
}
const borderEdges = (soup: Float32Array): string[] => edgesUsed(soup, (n) => n === 1);
function area(soup: Float32Array): number {
  let total = 0;
  const a = new Vector3(), b = new Vector3(), c = new Vector3();
  for (let t = 0; t < soup.length; t += 9) {
    a.fromArray(soup, t); b.fromArray(soup, t + 3).sub(a); c.fromArray(soup, t + 6).sub(a);
    total += 0.5 * b.cross(c).length();
  }
  return total;
}
/** the largest distance from the corners and centroids of `from` to the surface `to` (three-mesh-bvh as the oracle) */
function farthest(from: Float32Array, to: Float32Array): number {
  const geometry = new BufferGeometry();
  geometry.setAttribute('position', new BufferAttribute(to.slice(), 3));
  const tree = new MeshBVH(geometry);
  const hit = { point: new Vector3(), distance: 0, faceIndex: 0 };
  const p = new Vector3();
  let worst = 0;
  for (let t = 0; t < from.length; t += 9) {
    for (let corner = 0; corner < 4; corner += 1) {
      if (corner < 3) p.fromArray(from, t + corner * 3);
      else p.set((from[t] + from[t + 3] + from[t + 6]) / 3, (from[t + 1] + from[t + 4] + from[t + 7]) / 3, (from[t + 2] + from[t + 5] + from[t + 8]) / 3);
      tree.closestPointToPoint(p, hit);
      worst = Math.max(worst, hit.distance);
    }
  }
  return worst;
}

describe('lossless indexed geometry', () => {
  it.each([
    ['a patch', () => Float32Array.from(sheet(20, 20, flat(0))), 441, Uint16Array],
    ['negative zero and zero, which differ in bits', () => Float32Array.from([0, 0, 0, 1, 0, 0, 0, 1, 0, -0, 0, 0, 1, 0, 0, 0, 1, 0]), 4, Uint16Array],
    ['the last size 16-bit indices can number', () => Float32Array.from({ length: 65535 * 3 }, (_, i) => i), 65535, Uint16Array],
    ['one triangle more', () => Float32Array.from({ length: 65538 * 3 }, (_, i) => i), 65538, Uint32Array],
    ['a large connected surface', () => Float32Array.from(sheet(300, 300, flat(0))), 301 * 301, Uint32Array],
  ] as const)('%s', (_, make, vertices, Indices) => {
    const soup = make();
    const mesh = indexTriangleSoup(soup);
    expect(mesh.positions.length / 3).toBe(vertices);
    expect(mesh.indices).toBeInstanceOf(Indices);
    expect(bits(expand(mesh))).toEqual(bits(soup));
  });

  it('is what the viewport uploads: the same surface in under 40% of the bytes of the soup, without normals', () => {
    const soup = Float32Array.from(sheet(40, 30, flat(0)));
    const geometry = surfaceMeshGeometry(soup);
    expect(Object.keys(geometry.attributes)).toEqual(['position']);
    const positions = geometry.attributes.position.array as Float32Array;
    const indices = geometry.index!.array as Uint16Array;
    expect(bits(expand({ positions, indices }))).toEqual(bits(soup));
    expect(positions.byteLength + indices.byteLength).toBeLessThan(soup.byteLength * 0.4);
    geometry.dispose();
  });
});

const SURFACES = [
  { name: 'a flat wall', soup: () => sheet(20, 20, flat(0)) },
  { name: 'a wall with a doorway', soup: () => sheet(24, 20, flat(0), (u, v) => u >= 9 && u < 15 && v < 14) },
  { name: 'a floor meeting a wall', soup: () => [...sheet(20, 16, flat(0)), ...sheet(20, 16, (u, v) => [u * CELL, 0, v * CELL])] },
  { name: 'a wall 3 cm thick with a window through both faces', soup: () => [0, 0.03].flatMap((z) => sheet(20, 20, flat(z), (u, v) => u >= 7 && u < 12 && v >= 7 && v < 12)) },
];

describe('the lighter copy for the scanner', () => {
  it.each(SURFACES)('$name: at most half the triangles, the same borders, the same surface', async ({ soup }) => {
    const input = Float32Array.from(soup());
    const { trianglePositions: output, simplified } = await simplifyMeshForDisplay(input, VOXEL);
    expect(simplified).toBe(true);
    expect(output.length).toBeLessThanOrEqual(input.length / 2);
    expect(borderEdges(output)).toEqual(borderEdges(input));
    const known = new Set(corners(input));
    expect(corners(output).every((corner) => known.has(corner))).toBe(true);
    expect(area(output)).toBeCloseTo(area(input), 4);
    expect(farthest(input, output)).toBeLessThan(1e-5);
    expect(farthest(output, input)).toBeLessThan(1e-5);
  });

  const noise = (u: number, v: number): number => 0.0004 * Math.sin(u * 7.1) * Math.cos(v * 5.3);

  it('keeps a noisy surface within the tolerance', async () => {
    const tolerance = VOXEL * DISPLAY_LOD_TOLERANCE_VOXELS;
    const input = Float32Array.from(sheet(20, 20, (u, v) => [u * CELL, v * CELL, noise(u, v)]));
    const { trianglePositions: output } = await simplifyMeshForDisplay(input, VOXEL);
    expect(output.length).toBeLessThan(input.length / 2);
    expect(farthest(input, output)).toBeLessThan(tolerance);
    expect(farthest(output, input)).toBeLessThan(tolerance);
  });

  it('limits the error as a length: studs on a mesh 30 m wide are not flattened', async () => {
    /* a limit relative to the size of the mesh would be about 7 cm here and would cut the 15 mm studs off */
    const stud = (u: number, v: number): number => (u % 4 === 2 && v % 4 === 2 ? 0.015 : 0);
    const input = Float32Array.from([...sheet(20, 20, flat(0)), ...sheet(20, 20, (u, v) => [30 + u * CELL, v * CELL, stud(u, v)])]);
    const { trianglePositions: output } = await simplifyMeshForDisplay(input, VOXEL);
    expect(output.length).toBeLessThan(input.length * 0.75);
    expect(farthest(input, output)).toBeLessThan(3 * VOXEL * DISPLAY_LOD_TOLERANCE_VOXELS);
  });

  it('keeps the tip of a feature far taller than the tolerance', async () => {
    const tip: Point = [10 * CELL, 10 * CELL, 0.03];
    const input = Float32Array.from(sheet(20, 20, (u, v) => (u === 10 && v === 10 ? tip : [u * CELL, v * CELL, noise(u, v)])));
    const { trianglePositions: output } = await simplifyMeshForDisplay(input, VOXEL);
    expect(output.length).toBeLessThan(input.length / 2);
    expect(corners(output)).toContain(corners(Float32Array.from(tip))[0]);
  });

  it('keeps every edge where three faces meet: a wall standing on the middle of a floor stays joined to it', async () => {
    /* without the vertex locks the simplifier takes the three sheets apart along the junction (20 shared edges became 2) */
    const input = Float32Array.from([...sheet(20, 20, (u, v) => [u * CELL, (v - 10) * CELL, 0]), ...sheet(20, 12, (u, v) => [u * CELL, 0, v * CELL])]);
    const junction = edgesUsed(input, (n) => n > 2);
    expect(junction).toHaveLength(20);
    const { trianglePositions: output } = await simplifyMeshForDisplay(input, VOXEL);
    expect(output.length).toBeLessThanOrEqual(input.length / 2);
    expect(edgesUsed(output, (n) => n > 2)).toEqual(junction);
    expect(borderEdges(output)).toEqual(borderEdges(input));
    expect(farthest(input, output)).toBeLessThan(1e-5);
  });

  const broken: Array<[string, () => Promise<SimplifierModule>]> = [
    ['the module cannot be loaded', () => Promise.reject(new Error('Failed to fetch dynamically imported module'))],
    ['WebAssembly is not supported', async () => ({ MeshoptSimplifier: { supported: false } }) as unknown as SimplifierModule],
    ['the WebAssembly does not compile', async () => ({ MeshoptSimplifier: { supported: true, ready: Promise.reject(new Error('CompileError')) } }) as unknown as SimplifierModule],
    ['the simplifier throws', async () => ({ MeshoptSimplifier: { supported: true, ready: Promise.resolve(), simplifyWithAttributes: () => { throw new Error('memory'); } } }) as unknown as SimplifierModule],
  ];
  it.each(broken)('gives the reconstructed mesh itself when %s', async (_, load) => {
    const input = Float32Array.from(sheet(20, 20, flat(0)));
    const result = await simplifyMeshForDisplay(input, VOXEL, load);
    expect(result.simplified).toBe(false);
    expect(result.trianglePositions).toBe(input);
  });
});
