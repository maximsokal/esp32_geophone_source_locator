import { describe, expect, it } from 'vitest';

import {
  detectLocalMeshNoiseHotspots,
  type MeshNoiseHotspotTriangle,
} from '../../src/xr/meshNoiseHotspots';

type Point = readonly [number, number, number];

function triangle(center: Point): MeshNoiseHotspotTriangle {
  return {
    vertices: [
      [center[0] - 0.006, center[1] - 0.004, center[2]],
      [center[0] + 0.006, center[1] - 0.004, center[2]],
      [center[0], center[1] + 0.008, center[2]],
    ],
  };
}

function componentTriangles(count = 24): MeshNoiseHotspotTriangle[] {
  return Array.from({ length: count }, (_, index) => triangle([
    (index % 6) * 0.012,
    Math.floor(index / 6) * 0.012,
    0,
  ]));
}

function labels(count: number): Int32Array {
  return new Int32Array(count);
}

describe('local mesh noise hotspots', () => {
  it('quarantines only a small spatial patch when self-intersection and normal intrusion agree', () => {
    const triangles = componentTriangles();
    const selfHits = new Uint16Array(triangles.length);
    const normalHits = new Uint8Array(triangles.length);
    selfHits[0] = 1;
    selfHits[1] = 1;
    normalHits[0] = 1;
    normalHits[2] = 1;
    normalHits[3] = 1;

    const result = detectLocalMeshNoiseHotspots(
      triangles,
      labels(triangles.length),
      1,
      selfHits,
      normalHits,
      0.04,
    );

    expect(result.hotspotCount).toBeGreaterThan(0);
    expect(result.quarantinedTriangleCount).toBe(4);
    expect(Array.from(result.quarantineMask.slice(0, 4))).toEqual([1, 1, 1, 1]);
    expect(Array.from(result.quarantineMask.slice(4)).some((value) => value !== 0)).toBe(false);
  });

  it('does not remove a thin double-surface signal from normal intrusion alone', () => {
    const triangles = componentTriangles();
    const normalHits = new Uint8Array(triangles.length);
    normalHits.fill(1, 0, 8);

    const result = detectLocalMeshNoiseHotspots(
      triangles,
      labels(triangles.length),
      1,
      new Uint16Array(triangles.length),
      normalHits,
      0.04,
    );

    expect(result.hotspotCount).toBe(0);
    expect(result.quarantinedTriangleCount).toBe(0);
  });

  it('leaves widespread correlated noise for component-level quarantine instead of hiding it as local repair', () => {
    const triangles = componentTriangles();
    const selfHits = new Uint16Array(triangles.length);
    const normalHits = new Uint8Array(triangles.length);
    for (let index = 0; index < 8; index += 1) {
      selfHits[index] = 1;
      normalHits[index] = 1;
    }

    const result = detectLocalMeshNoiseHotspots(
      triangles,
      labels(triangles.length),
      1,
      selfHits,
      normalHits,
      0.04,
    );

    expect(result.hotspotCount).toBe(0);
    expect(result.quarantinedTriangleCount).toBe(0);
  });

  it('finds one local patch even when suspicious faces straddle a spatial hash boundary', () => {
    const triangles = componentTriangles();
    triangles[0] = triangle([0.098, 0, 0]);
    triangles[1] = triangle([0.102, 0, 0]);
    triangles[2] = triangle([0.099, 0.012, 0]);
    triangles[3] = triangle([0.103, 0.012, 0]);
    const selfHits = new Uint16Array(triangles.length);
    const normalHits = new Uint8Array(triangles.length);
    selfHits[0] = 1;
    selfHits[1] = 1;
    normalHits[0] = 1;
    normalHits[2] = 1;
    normalHits[3] = 1;

    const result = detectLocalMeshNoiseHotspots(
      triangles,
      labels(triangles.length),
      1,
      selfHits,
      normalHits,
      0.04,
    );

    expect(result.quarantinedTriangleCount).toBe(4);
  });
});
