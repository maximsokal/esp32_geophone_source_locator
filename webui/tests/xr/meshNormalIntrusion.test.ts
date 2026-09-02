import { describe, expect, it } from 'vitest';

import {
  diagnoseMeshNormalIntrusions,
  MAX_NORMAL_INTRUSION_EXACT_TESTS,
  type MeshNormalIntrusionTriangle,
} from '../../src/xr/meshNormalIntrusion';

type Point = readonly [number, number, number];

function vertexKey(point: Point): string {
  return `${point[0].toFixed(5)}:${point[1].toFixed(5)}:${point[2].toFixed(5)}`;
}

function triangle(a: Point, b: Point, c: Point): MeshNormalIntrusionTriangle {
  return { vertices: [a, b, c], keys: [vertexKey(a), vertexKey(b), vertexKey(c)] };
}

function horizontalPatch(z: number, cells = 3, spacing = 0.04): MeshNormalIntrusionTriangle[] {
  const output: MeshNormalIntrusionTriangle[] = [];
  for (let y = 0; y < cells; y += 1) {
    for (let x = 0; x < cells; x += 1) {
      const x0 = x * spacing; const x1 = x0 + spacing;
      const y0 = y * spacing; const y1 = y0 + spacing;
      output.push(
        triangle([x0, y0, z], [x1, y0, z], [x1, y1, z]),
        triangle([x0, y0, z], [x1, y1, z], [x0, y1, z]),
      );
    }
  }
  return output;
}

function weldedRightAngleFold(): MeshNormalIntrusionTriangle[] {
  const floor = horizontalPatch(0, 3, 0.04);
  const wall: MeshNormalIntrusionTriangle[] = [];
  for (let z = 0; z < 3; z += 1) {
    for (let x = 0; x < 3; x += 1) {
      const x0 = x * 0.04; const x1 = x0 + 0.04;
      const z0 = z * 0.04; const z1 = z0 + 0.04;
      wall.push(
        triangle([x0, 0, z0], [x1, 0, z1], [x1, 0, z0]),
        triangle([x0, 0, z0], [x0, 0, z1], [x1, 0, z1]),
      );
    }
  }
  return [...floor, ...wall];
}

function componentLabels(count: number, splitAt?: number): Int32Array {
  const labels = new Int32Array(count);
  if (splitAt !== undefined) for (let index = splitAt; index < count; index += 1) labels[index] = 1;
  return labels;
}

describe('bounded mesh normal intrusion diagnostic', () => {
  it('does not report normal intrusions on one flat manifold patch', () => {
    const triangles = horizontalPatch(0);
    const result = diagnoseMeshNormalIntrusions(triangles, componentLabels(triangles.length), 1, 0.04);
    expect(result.intrudingTriangleCount).toBe(0);
    expect(result.truncated).toBe(false);
  });

  it('does not treat a welded 90-degree structural fold as normal-piercing noise', () => {
    const triangles = weldedRightAngleFold();
    const result = diagnoseMeshNormalIntrusions(triangles, componentLabels(triangles.length), 1, 0.04);
    expect(result.intrudingTriangleCount).toBe(0);
  });

  it('detects a nearby disconnected parallel shell without deleting either shell itself', () => {
    const lower = horizontalPatch(0);
    const upper = horizontalPatch(0.045);
    const triangles = [...lower, ...upper];
    const result = diagnoseMeshNormalIntrusions(
      triangles,
      componentLabels(triangles.length, lower.length),
      2,
      0.04,
    );
    expect(result.intrudingTriangleCount).toBeGreaterThan(0);
    expect(result.componentStats[0].intrudingTriangleRatio).toBeGreaterThan(0);
    expect(result.componentStats[1].intrudingTriangleRatio).toBeGreaterThan(0);
  });

  it('hard-bounds exact tests for pathological nearby triangle soup', () => {
    const triangles: MeshNormalIntrusionTriangle[] = [];
    for (let layer = 0; layer < 28; layer += 1) {
      triangles.push(...horizontalPatch(layer * 0.0025, 5, 0.02));
    }
    const result = diagnoseMeshNormalIntrusions(
      triangles,
      componentLabels(triangles.length),
      1,
      0.035,
    );
    expect(result.exactTestedPairs).toBeLessThanOrEqual(MAX_NORMAL_INTRUSION_EXACT_TESTS);
    expect(result.truncated || result.intrudingTriangleCount > 0).toBe(true);
  });
});
