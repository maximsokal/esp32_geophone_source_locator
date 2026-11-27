import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';

import { buildSparseTsdfMesh } from '../../src/xr/sparseTsdf';
import { TSDF_BLOCK_VOXELS, TsdfBlockGrid } from '../../src/xr/tsdfBlockGrid';

const GOLDEN = { nodes: 5992, triangles: 8706, raw: 8706, sha256: '462ff27ccb9482566d85a5e7' };

function random(seed: number): () => number { let state = seed >>> 0; return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 4294967296; }; }

/** The voxel coordinates a stored slot stands for. */
function coordinates(grid: TsdfBlockGrid, slot: number): [number, number, number] {
  const block = grid.blocks[Math.floor(slot / TSDF_BLOCK_VOXELS)]; const at = slot % TSDF_BLOCK_VOXELS;
  return [block.x * 8 + (at >> 6), block.y * 8 + ((at >> 3) & 7), block.z * 8 + (at & 7)];
}

describe('TSDF voxel block grid', () => {
  it.each([['no cap', 1_000_000], ['a cap on the number of voxels', 700]])('holds exactly what a plain per-voxel map holds, in the order of creation (%s)', (_name, cap) => {
    const next = random(42);
    const grid = new TsdfBlockGrid(cap);
    const reference = new Map<string, { value: number; weight: number }>();
    for (let i = 0; i < 20_000; i += 1) {
      /* around the origin, so that coordinates are negative and cross block borders; repeated often, so that sums accumulate */
      const c = [0, 1, 2].map(() => Math.floor(next() * 22) - 11) as [number, number, number];
      const value = next() * 2 - 1; const weight = 0.01 + next();
      grid.add(c[0], c[1], c[2], value, weight);
      const key = c.join(':'); const existing = reference.get(key);
      if (existing === undefined) { if (reference.size < cap) reference.set(key, { value: value * weight, weight }); }
      else { existing.value += value * weight; existing.weight += weight; }
    }
    const stored = Array.from({ length: grid.voxelCount }, (_, n) => {
      const slot = grid.voxelAt(n); const block = grid.blocks[Math.floor(slot / TSDF_BLOCK_VOXELS)];
      return [coordinates(grid, slot).join(':'), block.weightedValue[slot % TSDF_BLOCK_VOXELS], block.weight[slot % TSDF_BLOCK_VOXELS]];
    });
    expect(stored).toEqual([...reference].map(([key, v]) => [key, v.value, v.weight]));          /* bit for bit and in the same order */
    expect(grid.blocks.every((block) => block.weight.some((w) => w > 0)), 'no block is allocated for a dropped voxel').toBe(true);
  });

  it('keeps growing its creation order past its first allocation and finds blocks far apart', () => {
    const grid = new TsdfBlockGrid(1_000_000);
    for (let i = 0; i < 12_000; i += 1) grid.add(i * 9 - 50_000, -i, i % 5, 0.5, 1);
    expect(grid.voxelCount).toBe(12_000);
    expect(coordinates(grid, grid.voxelAt(11_999))).toEqual([11_999 * 9 - 50_000, -11_999, 4]);
    expect(grid.block(1e6, 0, 0)).toBeUndefined();
  });

  /* The expected values were produced by the per-voxel map kernel this grid replaced (commit 15bd7a48): same surface, same triangles, same order. */
  it('gives bit for bit the mesh of the per-voxel map on a scene with negative coordinates that spans many blocks', () => {
    const points: number[] = []; const normals: number[] = [];
    for (let a = 0; a < 30; a += 1) for (let b = 0; b < 30; b += 1) {
      points.push(a * 0.031 - 0.47, b * 0.031 - 0.47, -0.21 + 0.002 * Math.sin(a + b)); normals.push(0, 0, 1);
      points.push(-0.47, a * 0.031 - 0.47, b * 0.031 - 0.21); normals.push(1, 0, 0);
      points.push(a * 0.031 - 0.47, -0.47, b * 0.031 - 0.21); normals.push(0, 1, 0);
    }
    const mesh = buildSparseTsdfMesh({ positions: Float32Array.from(points), normals: Float32Array.from(normals), confidence: new Float32Array(points.length / 3).fill(0.9), voxelSizeMeters: 0.048 });
    const bytes = new Uint8Array(mesh.trianglePositions.buffer, mesh.trianglePositions.byteOffset, mesh.trianglePositions.byteLength);
    expect({ nodes: mesh.nodeCount, triangles: mesh.triangleCount, raw: mesh.rawTriangleCount, sha256: createHash('sha256').update(bytes).digest('hex').slice(0, 24) }).toEqual(GOLDEN);
  });
});
