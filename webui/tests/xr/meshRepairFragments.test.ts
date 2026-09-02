import { describe, expect, it } from 'vitest';

import {
  quarantineDetachedMeshRepairFragments,
  type MeshRepairPoint,
  type MeshRepairTriangle,
} from '../../src/xr/meshRepairFragments';

function pointKey(point: MeshRepairPoint): string {
  return `${point[0].toFixed(4)}:${point[1].toFixed(4)}:${point[2].toFixed(4)}`;
}

function triangle(a: MeshRepairPoint, b: MeshRepairPoint, c: MeshRepairPoint): MeshRepairTriangle {
  return { vertices: [a, b, c], keys: [pointKey(a), pointKey(b), pointKey(c)] };
}

function addCell(
  output: MeshRepairTriangle[],
  x: number,
  y: number,
  spacing = 0.04,
): readonly [number, number] {
  const start = output.length;
  const x0 = x * spacing; const x1 = x0 + spacing;
  const y0 = y * spacing; const y1 = y0 + spacing;
  output.push(
    triangle([x0, y0, 0], [x1, y0, 0], [x1, y1, 0]),
    triangle([x0, y0, 0], [x1, y1, 0], [x0, y1, 0]),
  );
  return [start, start + 1];
}

function grid(width: number, height: number): MeshRepairTriangle[] {
  const output: MeshRepairTriangle[] = [];
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) addCell(output, x, y);
  }
  return output;
}

function allComponent(count: number): Int32Array {
  return new Int32Array(count);
}

function repairable(): Uint8Array {
  return Uint8Array.of(1);
}

describe('post-hotspot mesh fragment cleanup', () => {
  it('preserves both substantial fragments when a removed noisy column splits one retained slab', () => {
    const triangles = grid(6, 3);
    const mask = new Uint8Array(triangles.length);
    // Column x=2: six triangles, 16.7% of the original 36-face slab.
    for (let y = 0; y < 3; y += 1) {
      const cell = y * 6 + 2;
      mask[cell * 2] = 1;
      mask[cell * 2 + 1] = 1;
    }

    const result = quarantineDetachedMeshRepairFragments(
      triangles,
      allComponent(triangles.length),
      1,
      mask,
      repairable(),
      0.04,
    );

    expect(result.revertedComponentCount).toBe(0);
    expect(result.detachedFragmentCount).toBe(0);
    expect(result.detachedTriangleCount).toBe(0);
    expect(Array.from(result.quarantineMask).filter(Boolean)).toHaveLength(6);
  });

  it('quarantines a tiny tab detached by local bridge cleanup while keeping the substantial slab', () => {
    const triangles = grid(4, 3); // 24-triangle substantial slab.
    const bridge: number[] = [];
    // Two 4-cm cells form a narrow bridge centred on the slab middle row.
    bridge.push(...addCell(triangles, 4, 1));
    bridge.push(...addCell(triangles, 5, 1));
    const tiny = [...addCell(triangles, 6, 1)];
    const mask = new Uint8Array(triangles.length);
    for (const index of bridge) mask[index] = 1;

    const result = quarantineDetachedMeshRepairFragments(
      triangles,
      allComponent(triangles.length),
      1,
      mask,
      repairable(),
      0.04,
    );

    expect(result.revertedComponentCount).toBe(0);
    expect(result.detachedFragmentCount).toBe(1);
    expect(result.detachedTriangleCount).toBe(2);
    expect(bridge.every((index) => result.quarantineMask[index] === 1)).toBe(true);
    expect(tiny.every((index) => result.quarantineMask[index] === 1)).toBe(true);
    expect(Array.from(result.quarantineMask).filter(Boolean)).toHaveLength(6);
  });

  it('reverts a destructive local repair when no substantial fragment would survive', () => {
    const triangles = grid(4, 1);
    const mask = new Uint8Array(triangles.length);
    // Remove the two centre cells. Remaining one-cell fragments each have only
    // two triangles, so this local repair would leave no meaningful mesh region.
    for (const cell of [1, 2]) {
      mask[cell * 2] = 1;
      mask[cell * 2 + 1] = 1;
    }

    const result = quarantineDetachedMeshRepairFragments(
      triangles,
      allComponent(triangles.length),
      1,
      mask,
      repairable(),
      0.04,
    );

    expect(result.revertedComponentCount).toBe(1);
    expect(result.detachedTriangleCount).toBe(0);
    expect(Array.from(result.quarantineMask).every((value) => value === 0)).toBe(true);
  });
});
