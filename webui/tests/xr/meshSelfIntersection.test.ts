import { describe, expect, it } from 'vitest';

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
    expect(diagnostic.broadPhaseCandidatePairs).toBe(0);
    expect(diagnostic.exactTestedPairs).toBe(0);
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
});
