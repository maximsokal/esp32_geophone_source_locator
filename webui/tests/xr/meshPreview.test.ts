import { describe, expect, it } from 'vitest';
import {
  buildSurfaceMeshPreview,
  triangleGeometryAcceptable,
} from '../../src/xr/meshPreview';

function planeGrid(
  width: number,
  height: number,
  spacing = 0.04,
  z = 0,
): { positions: Float32Array; normals: Float32Array; confidence: Float32Array } {
  const positions = new Float32Array(width * height * 3);
  const normals = new Float32Array(width * height * 3);
  const confidence = new Float32Array(width * height);
  let point = 0;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = point * 3;
      positions[offset] = x * spacing;
      positions[offset + 1] = y * spacing;
      positions[offset + 2] = z;
      normals[offset] = 0; normals[offset + 1] = 0; normals[offset + 2] = 1;
      confidence[point] = 0.9;
      point += 1;
    }
  }
  return { positions, normals, confidence };
}

describe('triangle geometry gate', () => {
  it('accepts a compact non-degenerate triangle', () => {
    expect(triangleGeometryAcceptable([0, 0, 0], [0.04, 0, 0], [0, 0.04, 0], 0.1)).toBe(true);
  });

  it('rejects an edge longer than the search radius', () => {
    expect(triangleGeometryAcceptable([0, 0, 0], [0.2, 0, 0], [0, 0.04, 0], 0.1)).toBe(false);
  });

  it('rejects degenerate collinear triangles by angle constraints', () => {
    expect(triangleGeometryAcceptable([0, 0, 0], [0.04, 0, 0], [0.08, 0, 0], 0.1)).toBe(false);
  });
});

describe('GP3-style mesh preview', () => {
  it('returns no triangles for an empty cloud', () => {
    const mesh = buildSurfaceMeshPreview({
      positions: new Float32Array(0), normals: new Float32Array(0),
      confidence: new Float32Array(0), voxelSizeMeters: 0.04,
    });
    expect(mesh.triangleCount).toBe(0);
  });

  it('builds local triangles on a dense planar point set', () => {
    const cloud = planeGrid(12, 12);
    const mesh = buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 0.04 });
    expect(mesh.triangleCount).toBeGreaterThan(100);
    expect(mesh.trianglePositions.length).toBe(mesh.triangleCount * 9);
  });

  it('filters low-confidence points before triangulation', () => {
    const cloud = planeGrid(8, 8);
    cloud.confidence.fill(0.2);
    const mesh = buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 0.04 });
    expect(mesh.acceptedPointCount).toBe(0);
    expect(mesh.triangleCount).toBe(0);
  });

  it('does not bridge two orthogonal surfaces solely because points are nearby', () => {
    const a = planeGrid(8, 8, 0.04, 0);
    const countA = a.confidence.length;
    const countB = countA;
    const positions = new Float32Array((countA + countB) * 3);
    const normals = new Float32Array((countA + countB) * 3);
    const confidence = new Float32Array(countA + countB);
    positions.set(a.positions); normals.set(a.normals); confidence.set(a.confidence);
    for (let i = 0; i < countB; i += 1) {
      const offset = (countA + i) * 3;
      const x = (i % 8) * 0.04;
      const z = Math.floor(i / 8) * 0.04;
      positions[offset] = x;
      positions[offset + 1] = 0;
      positions[offset + 2] = z;
      normals[offset] = 0; normals[offset + 1] = 1; normals[offset + 2] = 0;
      confidence[countA + i] = 0.9;
    }
    const mesh = buildSurfaceMeshPreview({ positions, normals, confidence, voxelSizeMeters: 0.04 });
    expect(mesh.triangleCount).toBeGreaterThan(20);
    for (let i = 0; i < mesh.trianglePositions.length; i += 9) {
      const tri = mesh.trianglePositions.subarray(i, i + 9);
      const allOnA = Math.abs(tri[2]) < 1e-5 && Math.abs(tri[5]) < 1e-5 && Math.abs(tri[8]) < 1e-5;
      const allOnB = Math.abs(tri[1]) < 1e-5 && Math.abs(tri[4]) < 1e-5 && Math.abs(tri[7]) < 1e-5;
      expect(allOnA || allOnB).toBe(true);
    }
  });

  it('keeps all generated coordinates finite', () => {
    const cloud = planeGrid(10, 10);
    const mesh = buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 0.04 });
    for (const value of mesh.trianglePositions) expect(Number.isFinite(value)).toBe(true);
  });

  it('is deterministic for the same point set', () => {
    const cloud = planeGrid(9, 9);
    const first = buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 0.04 });
    const second = buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 0.04 });
    expect(Array.from(first.trianglePositions)).toEqual(Array.from(second.trianglePositions));
  });

  it('caps the reconstruction search radius for unusually coarse voxels', () => {
    const cloud = planeGrid(5, 5, 0.1);
    const mesh = buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 1.0 });
    expect(mesh.searchRadiusMeters).toBeLessThanOrEqual(0.18);
  });

  it('does not mutate source position buffers', () => {
    const cloud = planeGrid(8, 8);
    const copy = Array.from(cloud.positions);
    buildSurfaceMeshPreview({ ...cloud, voxelSizeMeters: 0.04 });
    expect(Array.from(cloud.positions)).toEqual(copy);
  });
});
