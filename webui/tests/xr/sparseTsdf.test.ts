import { describe, expect, it } from 'vitest';

import { buildSparseTsdfMesh } from '../../src/xr/sparseTsdf';

interface Cloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): Cloud { return { positions: [], normals: [], confidence: [] }; }

function addPlane(
  target: Cloud,
  centerX: number,
  normal: readonly [number, number, number] = [0, 0, 1],
  confidence = 0.94,
): void {
  for (let ix = -4; ix <= 4; ix += 1) {
    for (let iy = -4; iy <= 4; iy += 1) {
      target.positions.push(centerX + ix * 0.04, iy * 0.04, 0);
      target.normals.push(...normal);
      target.confidence.push(confidence);
    }
  }
}

function input(source: Cloud) {
  return {
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    voxelSizeMeters: 0.04,
    freeSpacePositions: Float32Array.from([
      0, 0, 0.18,
      0.8, 0, 0.18,
    ]),
    freeSpaceVoxelSizeMeters: 0.18,
  };
}

describe('sparse weighted TSDF reconstruction', () => {
  it('extracts a bounded zero-crossing mesh from a stable oriented plane', () => {
    const source = cloud();
    addPlane(source, 0);
    const mesh = buildSparseTsdfMesh(input(source));

    expect(mesh.integratedPointCount).toBeGreaterThan(60);
    expect(mesh.nodeCount).toBeGreaterThan(100);
    expect(mesh.triangleCount).toBeGreaterThan(20);
    let maxAbsZ = 0;
    for (let offset = 2; offset < mesh.trianglePositions.length; offset += 3) {
      maxAbsZ = Math.max(maxAbsZ, Math.abs(mesh.trianglePositions[offset]));
    }
    expect(maxAbsZ).toBeLessThan(mesh.voxelSizeMeters * 0.35);
  });

  it('does not manufacture a surface from low-confidence points', () => {
    const source = cloud();
    addPlane(source, 0, [0, 0, 1], 0.30);
    const mesh = buildSparseTsdfMesh(input(source));
    expect(mesh.integratedPointCount).toBe(0);
    expect(mesh.triangleCount).toBe(0);
  });

  it('does not bridge two disconnected measured surface islands', () => {
    const source = cloud();
    addPlane(source, -0.55);
    addPlane(source, 0.55);
    const mesh = buildSparseTsdfMesh(input(source));
    expect(mesh.triangleCount).toBeGreaterThan(30);

    let maximumTriangleSpan = 0;
    for (let offset = 0; offset + 8 < mesh.trianglePositions.length; offset += 9) {
      const xs = [
        mesh.trianglePositions[offset],
        mesh.trianglePositions[offset + 3],
        mesh.trianglePositions[offset + 6],
      ];
      maximumTriangleSpan = Math.max(maximumTriangleSpan, Math.max(...xs) - Math.min(...xs));
    }
    expect(maximumTriangleSpan).toBeLessThan(0.16);
  });

  it('respects the triangle budget', () => {
    const source = cloud();
    addPlane(source, 0);
    const mesh = buildSparseTsdfMesh({ ...input(source), maxTriangles: 12 });
    expect(mesh.triangleCount).toBeLessThanOrEqual(12);
  });
});
