import { describe, expect, it } from 'vitest';

import {
  quarantineSubscaleMeshBranches,
  type MeshBranchPoint,
  type MeshBranchTriangle,
} from '../../src/xr/meshThinBranches';

function pointKey(point: MeshBranchPoint): string {
  return `${point[0].toFixed(5)}:${point[1].toFixed(5)}:${point[2].toFixed(5)}`;
}

function triangle(a: MeshBranchPoint, b: MeshBranchPoint, c: MeshBranchPoint): MeshBranchTriangle {
  return { vertices: [a, b, c], keys: [pointKey(a), pointKey(b), pointKey(c)] };
}

function gridPatch(cellsX: number, cellsY: number, spacing = 0.04): MeshBranchTriangle[] {
  const output: MeshBranchTriangle[] = [];
  for (let y = 0; y < cellsY; y += 1) {
    for (let x = 0; x < cellsX; x += 1) {
      const x0 = x * spacing;
      const x1 = x0 + spacing;
      const y0 = y * spacing;
      const y1 = y0 + spacing;
      output.push(
        triangle([x0, y0, 0], [x1, y0, 0], [x1, y1, 0]),
        triangle([x0, y0, 0], [x1, y1, 0], [x0, y1, 0]),
      );
    }
  }
  return output;
}

function labels(count: number): Int32Array {
  return new Int32Array(count);
}

function thinWhisker(): MeshBranchTriangle[] {
  // First face shares the right boundary edge of a 5x3, 4-cm grid. The rest
  // tapers into a long narrow chain. Total area stays below one voxel squared
  // while longitudinal extent is >4 voxels.
  const a: MeshBranchPoint = [0.20, 0.00, 0];
  const b: MeshBranchPoint = [0.20, 0.04, 0];
  const c: MeshBranchPoint = [0.24, 0.02, 0];
  const d: MeshBranchPoint = [0.28, 0.018, 0];
  const e: MeshBranchPoint = [0.32, 0.022, 0];
  const f: MeshBranchPoint = [0.36, 0.018, 0];
  return [
    triangle(a, b, c),
    triangle(b, d, c),
    triangle(c, d, e),
    triangle(d, f, e),
  ];
}

function broadExtension(): MeshBranchTriangle[] {
  // Same attachment edge, but a proper 4-cm-wide measured continuation. Even if
  // the shared-edge graph has a bridge at the attachment, this is not sub-voxel.
  return [
    triangle([0.20, 0.00, 0], [0.20, 0.04, 0], [0.24, 0.04, 0]),
    triangle([0.20, 0.00, 0], [0.24, 0.04, 0], [0.24, 0.00, 0]),
    triangle([0.24, 0.00, 0], [0.24, 0.04, 0], [0.28, 0.04, 0]),
    triangle([0.24, 0.00, 0], [0.28, 0.04, 0], [0.28, 0.00, 0]),
  ];
}

function syntheticDeepChain(count: number): MeshBranchTriangle[] {
  const output: MeshBranchTriangle[] = [];
  for (let index = 0; index < count; index += 1) {
    const a: MeshBranchPoint = [index * 0.002, 0, 0];
    const b: MeshBranchPoint = [(index + 1) * 0.002, 0.004, 0];
    const c: MeshBranchPoint = [(index + 2) * 0.002, 0, 0];
    output.push({
      vertices: [a, b, c],
      // Consecutive triangles share exactly the last two graph vertices. Using
      // explicit graph keys isolates the iterative-Tarjan stress from geometry.
      keys: [`k${index}`, `k${index + 1}`, `k${index + 2}`],
    });
  }
  return output;
}

describe('subscale attached mesh branches', () => {
  it('quarantines a long sub-voxel whisker attached to a substantial retained patch', () => {
    const base = gridPatch(5, 3);
    const whisker = thinWhisker();
    const triangles = [...base, ...whisker];
    const result = quarantineSubscaleMeshBranches(
      triangles,
      labels(triangles.length),
      1,
      Uint8Array.of(1),
      0.04,
    );

    expect(result.branchCount).toBe(1);
    expect(result.quarantinedTriangleCount).toBe(whisker.length);
    expect(Array.from(result.quarantineMask.slice(0, base.length))).toEqual(new Array(base.length).fill(0));
    expect(Array.from(result.quarantineMask.slice(base.length))).toEqual(new Array(whisker.length).fill(1));
  });

  it('does not shave ordinary grid boundary caps merely because their adjacency contains bridges', () => {
    const triangles = gridPatch(5, 3);
    const result = quarantineSubscaleMeshBranches(
      triangles,
      labels(triangles.length),
      1,
      Uint8Array.of(1),
      0.04,
    );
    expect(result.branchCount).toBe(0);
    expect(result.quarantinedTriangleCount).toBe(0);
  });

  it('retains a broad attached continuation that is physically wider than the subscale gate', () => {
    const base = gridPatch(5, 3);
    const extension = broadExtension();
    const triangles = [...base, ...extension];
    const result = quarantineSubscaleMeshBranches(
      triangles,
      labels(triangles.length),
      1,
      Uint8Array.of(1),
      0.04,
    );
    expect(result.branchCount).toBe(0);
    expect(result.quarantinedTriangleCount).toBe(0);
  });

  it('uses iterative bridge traversal for a deep chain instead of recursive JS call-stack depth', () => {
    const triangles = syntheticDeepChain(2400);
    expect(() => quarantineSubscaleMeshBranches(
      triangles,
      labels(triangles.length),
      1,
      Uint8Array.of(1),
      0.04,
    )).not.toThrow();
  });

  it('never operates on a component that has not already passed the retain gate', () => {
    const triangles = [...gridPatch(5, 3), ...thinWhisker()];
    const result = quarantineSubscaleMeshBranches(
      triangles,
      labels(triangles.length),
      1,
      Uint8Array.of(0),
      0.04,
    );
    expect(result.branchCount).toBe(0);
    expect(result.quarantinedTriangleCount).toBe(0);
  });
});
