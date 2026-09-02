import { describe, expect, it } from 'vitest';

import {
  buildDirectionalTsdfMesh,
  finalizeDirectionalTsdfPlan,
  prepareDirectionalTsdfPlan,
} from '../../src/xr/directionalTsdf';
import { buildSparseTsdfMesh } from '../../src/xr/sparseTsdf';

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

function appendTinyPatch(
  positions: number[], normals: number[], confidence: number[],
  countPerAxis = 10,
): void {
  for (let y = 0; y < countPerAxis; y += 1) {
    for (let x = 0; x < countPerAxis; x += 1) {
      positions.push(x * 0.006, y * 0.006, 0);
      normals.push(0, 0, 1);
      confidence.push(0.92);
    }
  }
}

describe('directional TSDF', () => {
  it('keeps orthogonal box faces in separate directional channels and sanitizes the merged mesh', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, 0, [1, 0, 0]);
    appendPlane(positions, normals, confidence, 1, 0, [0, 1, 0]);
    appendPlane(positions, normals, confidence, 2, 0, [0, 0, 1]);
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04, maxTriangles: 12000,
    });
    expect(mesh.directionalChannelCount).toBeGreaterThanOrEqual(3);
    expect(mesh.rawTriangleCount).toBeGreaterThanOrEqual(mesh.triangleCount);
    expect(mesh.triangleCount).toBeGreaterThan(0);
    expect(mesh.triangleCount).toBeLessThanOrEqual(12000);
    expect(mesh.eligibleComponentCount).toBeGreaterThanOrEqual(3);
    expect(mesh.meshReadiness).toBe(1);
    expect(mesh.orientationConflictCount).toBe(0);
    expect(mesh.meshTopologyComponentCount).toBeGreaterThan(0);
    expect(Number.isFinite(mesh.localNoiseHotspotCount)).toBe(true);
    expect(Number.isFinite(mesh.localNoiseHotspotTriangleCount)).toBe(true);
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
    expect(mesh.rawTriangleCount).toBeGreaterThanOrEqual(mesh.triangleCount);
    expect(mesh.orientationConflictCount).toBe(0);
  });

  it('still obeys the total triangle budget across channels before and after sanitation', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, 0, [1, 0, 0]);
    appendPlane(positions, normals, confidence, 1, 0, [0, 1, 0]);
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04, maxTriangles: 600,
    });
    expect(mesh.rawTriangleCount).toBeLessThanOrEqual(600);
    expect(mesh.triangleCount).toBeLessThanOrEqual(600);
  });

  it('keeps explicit plan -> channel -> finalize equivalent to the sequential wrapper', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, 0, [1, 0, 0]);
    appendPlane(positions, normals, confidence, 1, 0, [0, 1, 0]);
    appendPlane(positions, normals, confidence, 2, 0, [0, 0, 1]);
    const input = {
      positions: Float32Array.from(positions),
      normals: Float32Array.from(normals),
      confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04,
      maxTriangles: 12000,
    };
    const sequential = buildDirectionalTsdfMesh({
      ...input,
      positions: input.positions.slice(),
      normals: input.normals.slice(),
      confidence: input.confidence.slice(),
    });
    const plan = prepareDirectionalTsdfPlan(input, true);
    const channelMeshes = plan.channelInputs.map((channelInput) => buildSparseTsdfMesh(channelInput));
    const planned = finalizeDirectionalTsdfPlan(plan, channelMeshes);

    expect(plan.mode).toBe('multi');
    expect(plan.directionalChannelCount).toBeGreaterThanOrEqual(3);
    for (const channelInput of plan.channelInputs) {
      expect(channelInput.freeSpacePositions).toBeUndefined();
      expect(channelInput.freeSpaceVoxelSizeMeters).toBeUndefined();
    }
    expect(planned.directionalChannelCount).toBe(sequential.directionalChannelCount);
    expect(planned.channelTriangleCounts).toEqual(sequential.channelTriangleCounts);
    expect(planned.triangleCount).toBe(sequential.triangleCount);
    expect(planned.rawTriangleCount).toBe(sequential.rawTriangleCount);
    expect(planned.localNoiseHotspotCount).toBe(sequential.localNoiseHotspotCount);
    expect(planned.localNoiseHotspotTriangleCount).toBe(sequential.localNoiseHotspotTriangleCount);
    expect(Array.from(planned.trianglePositions)).toEqual(Array.from(sequential.trianglePositions));
  });

  it('does not build a directional mesh from many points on a physically tiny patch', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendTinyPatch(positions, normals, confidence);
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions),
      normals: Float32Array.from(normals),
      confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04,
    });
    expect(mesh.sourcePointCount).toBe(100);
    expect(mesh.meshReadiness).toBeLessThan(1);
    expect(mesh.eligibleComponentCount).toBe(0);
    expect(mesh.integratedPointCount).toBe(0);
    expect(mesh.rawTriangleCount).toBe(0);
    expect(mesh.triangleCount).toBe(0);
  });

  it('does not let an undersized conflicting normal island contaminate one mature directional surface', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 2, 0, [0, 0, 1]);
    for (let index = 0; index < 20; index += 1) {
      positions.push(0.01 * (index % 5), 0.01 * Math.floor(index / 5), 0.015);
      normals.push(1, 0, 0);
      confidence.push(0.92);
    }
    const mesh = buildDirectionalTsdfMesh({
      positions: Float32Array.from(positions),
      normals: Float32Array.from(normals),
      confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04,
    });
    expect(mesh.sourcePointCount).toBe(101);
    expect(mesh.directionalChannelCount).toBe(1);
    expect(mesh.eligibleComponentCount).toBe(1);
    expect(mesh.meshReadiness).toBe(1);
    expect(mesh.rejectedComponentPoints).toBeGreaterThanOrEqual(20);
    expect(mesh.rawTriangleCount).toBeGreaterThanOrEqual(mesh.triangleCount);
    expect(mesh.triangleCount).toBeGreaterThan(0);
  });
});
