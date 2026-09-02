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

function addTinyDensePlane(target: Cloud, originX = 0): void {
  for (let ix = 0; ix < 10; ix += 1) {
    for (let iy = 0; iy < 10; iy += 1) {
      target.positions.push(originX + ix * 0.006, iy * 0.006, 0);
      target.normals.push(0, 0, 1);
      target.confidence.push(0.94);
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

    expect(mesh.eligibleComponentCount).toBe(1);
    expect(mesh.meshReadiness).toBe(1);
    expect(mesh.integratedPointCount).toBeGreaterThan(60);
    expect(mesh.nodeCount).toBeGreaterThan(100);
    expect(mesh.rawTriangleCount).toBeGreaterThanOrEqual(mesh.triangleCount);
    expect(mesh.triangleCount).toBeGreaterThan(20);
    expect(mesh.orientationConflictCount).toBe(0);
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
    expect(mesh.rawTriangleCount).toBe(0);
  });

  it('does not manufacture a mesh from many points packed into a physically tiny island', () => {
    const source = cloud();
    addTinyDensePlane(source);
    const mesh = buildSparseTsdfMesh(input(source));
    expect(mesh.sourcePointCount).toBe(100);
    expect(mesh.componentCount).toBe(1);
    expect(mesh.eligibleComponentCount).toBe(0);
    expect(mesh.meshReadiness).toBeLessThan(1);
    expect(mesh.rejectedComponentPoints).toBe(100);
    expect(mesh.integratedPointCount).toBe(0);
    expect(mesh.triangleCount).toBe(0);
  });

  it('integrates the valid slab but rejects a disconnected dense tiny island inside the worker', () => {
    const source = cloud();
    addPlane(source, 0);
    addTinyDensePlane(source, 1.2);
    const mesh = buildSparseTsdfMesh(input(source));

    expect(mesh.sourcePointCount).toBe(181);
    expect(mesh.componentCount).toBe(2);
    expect(mesh.eligibleComponentCount).toBe(1);
    expect(mesh.rejectedComponentPoints).toBe(100);
    expect(mesh.integratedPointCount).toBeLessThanOrEqual(81);
    expect(mesh.integratedPointCount).toBeGreaterThan(60);
    expect(mesh.triangleCount).toBeGreaterThan(20);

    // No retained triangle may jump into the rejected island around x=1.2m.
    let maximumX = Number.NEGATIVE_INFINITY;
    for (let offset = 0; offset < mesh.trianglePositions.length; offset += 3) {
      maximumX = Math.max(maximumX, mesh.trianglePositions[offset]);
    }
    expect(maximumX).toBeLessThan(0.5);
  });

  it('does not bridge two disconnected measured surface islands', () => {
    const source = cloud();
    addPlane(source, -0.55);
    addPlane(source, 0.55);
    const mesh = buildSparseTsdfMesh(input(source));
    expect(mesh.eligibleComponentCount).toBe(2);
    expect(mesh.meshTopologyComponentCount).toBeGreaterThanOrEqual(2);
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

  it('respects the triangle budget before and after topology sanitation', () => {
    const source = cloud();
    addPlane(source, 0);
    const mesh = buildSparseTsdfMesh({ ...input(source), maxTriangles: 12 });
    expect(mesh.rawTriangleCount).toBeLessThanOrEqual(12);
    expect(mesh.triangleCount).toBeLessThanOrEqual(12);
  });
});
