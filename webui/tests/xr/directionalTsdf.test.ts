import { describe, expect, it } from 'vitest';

import { buildDirectionalTsdfMesh } from '../../src/xr/directionalTsdf';

function appendPlane(
  positions: number[], normals: number[], confidence: number[],
  axis: 0 | 1 | 2, value: number, normal: readonly [number, number, number],
): void {
  for (let a = -4; a <= 4; a += 1) {
    for (let b = -4; b <= 4; b += 1) {
      const point = [a * 0.04, b * 0.04, 0] as number[];
      if (axis === 0) { point[0] = value; point[1] = a * 0.04; point[2] = b * 0.04; }
      if (axis === 1) { point[0] = a * 0.04; point[1] = value; point[2] = b * 0.04; }
      if (axis === 2) { point[0] = a * 0.04; point[1] = b * 0.04; point[2] = value; }
      positions.push(...point); normals.push(...normal); confidence.push(0.92);
    }
  }
}

describe('directional TSDF', () => {
  it('keeps orthogonal box faces in separate directional channels', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, 0, [1, 0, 0]);
    appendPlane(positions, normals, confidence, 1, 0, [0, 1, 0]);
    appendPlane(positions, normals, confidence, 2, 0, [0, 0, 1]);
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04, maxTriangles: 12000,
    });
    expect(mesh.directionalChannelCount).toBeGreaterThanOrEqual(3);
    expect(mesh.triangleCount).toBeGreaterThan(0);
    expect(mesh.triangleCount).toBeLessThanOrEqual(12000);
  });

  it('keeps opposite thin surfaces separated instead of averaging them together', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, -0.035, [-1, 0, 0]);
    appendPlane(positions, normals, confidence, 0, 0.035, [1, 0, 0]);
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.035, maxTriangles: 10000,
    });
    expect(mesh.directionalChannelCount).toBe(2);
    expect(mesh.channelTriangleCounts.filter((count) => count > 0).length).toBe(2);
  });

  it('still obeys the total triangle budget across channels', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, 0, [1, 0, 0]);
    appendPlane(positions, normals, confidence, 1, 0, [0, 1, 0]);
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04, maxTriangles: 600,
    });
    expect(mesh.triangleCount).toBeLessThanOrEqual(600);
  });
});
