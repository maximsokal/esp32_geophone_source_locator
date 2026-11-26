import { describe, expect, it } from 'vitest';

import {
  buildDirectionalTsdfMesh,
  finalizeDirectionalTsdfPlan,
  prepareDirectionalTsdfPlan,
  prepareHybridTsdfPlan,
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

/* The tests of this describe are about the six-channel directional pass itself: they call it directly. The hybrid pipeline has its own describe below. */
const directionalOnly = (input: Parameters<typeof buildDirectionalTsdfMesh>[0]) => buildDirectionalTsdfMesh(input, { hybrid: false });

describe('directional TSDF', () => {
  it('keeps orthogonal box faces in separate directional channels and sanitizes the merged mesh', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendPlane(positions, normals, confidence, 0, 0, [1, 0, 0]);
    appendPlane(positions, normals, confidence, 1, 0, [0, 1, 0]);
    appendPlane(positions, normals, confidence, 2, 0, [0, 0, 1]);
    const mesh = directionalOnly({
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
    const mesh = directionalOnly({
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
    const mesh = directionalOnly({
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
    const sequential = directionalOnly({
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

  it('uses the scalar TSDF minimum FREE cell size before dropping FREE from transfer-safe channels', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    // One mature -X family at x=0. FREE sits 10 cm on +X. With the requested
    // 2-cm FREE voxel it lies outside the old directional planner's 7-cm search,
    // but the scalar TSDF intentionally clamps FREE cells to at least 4 cm and
    // therefore sees the same measurement within its 14-cm local orientation halo.
    appendPlane(positions, normals, confidence, 0, 0, [-1, 0, 0]);
    const free: number[] = [];
    for (let a = -4; a <= 4; a += 1) {
      for (let b = -4; b <= 4; b += 1) free.push(0.10, a * 0.04, b * 0.04);
    }
    const plan = prepareDirectionalTsdfPlan({
      positions: Float32Array.from(positions),
      normals: Float32Array.from(normals),
      confidence: Float32Array.from(confidence),
      voxelSizeMeters: 0.04,
      freeSpacePositions: Float32Array.from(free),
      freeSpaceVoxelSizeMeters: 0.02,
    }, true);

    expect(plan.mode).toBe('single');
    expect(plan.channelInputs).toHaveLength(1);
    expect(plan.channelInputs[0].freeSpacePositions).toBeUndefined();
    for (let offset = 0; offset < plan.channelInputs[0].normals.length; offset += 3) {
      expect(plan.channelInputs[0].normals[offset]).toBeGreaterThan(0.99);
      expect(Math.abs(plan.channelInputs[0].normals[offset + 1])).toBeLessThan(1e-6);
      expect(Math.abs(plan.channelInputs[0].normals[offset + 2])).toBeLessThan(1e-6);
    }
  });

  it('does not build a directional mesh from many points on a physically tiny patch', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    appendTinyPatch(positions, normals, confidence);
    const mesh = directionalOnly({
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
    const mesh = directionalOnly({
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

describe('hybrid reconstruction: scalar surface first, directional pass only for what it left uncovered', () => {
  const planes = (spec: Array<[0 | 1 | 2, number, readonly [number, number, number]]>) => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    for (const [axis, value, normal] of spec) appendPlane(positions, normals, confidence, axis, value, normal);
    return { positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence), voxelSizeMeters: 0.04, maxTriangles: 12000 };
  };

  it('is the default of buildDirectionalTsdfMesh, and an explicit plan -> channels -> finalize gives the very same mesh as the wrapper', () => {
    const input = planes([[0, 0, [1, 0, 0]], [1, 0, [0, 1, 0]], [2, 0, [0, 0, 1]]]);
    const wrapped = buildDirectionalTsdfMesh({ ...input, positions: input.positions.slice(), normals: input.normals.slice(), confidence: input.confidence.slice() });
    const plan = prepareHybridTsdfPlan(input, true);
    const { channelInputs, ...summary } = plan;
    const planned = finalizeDirectionalTsdfPlan(summary, channelInputs.map((channelInput) => buildSparseTsdfMesh(channelInput)));
    expect(planned.triangleCount).toBe(wrapped.triangleCount);
    expect(Array.from(planned.trianglePositions)).toEqual(Array.from(wrapped.trianglePositions));
    expect(planned.directionalFallbackPoints).toBe(wrapped.directionalFallbackPoints);
    expect(plan.baseMesh).toBeDefined();
  });

  it('a surface the scalar field covers is not handed to the directional pass at all', () => {
    const input = planes([[0, 0, [1, 0, 0]], [1, 0, [0, 1, 0]], [2, 0, [0, 0, 1]]]);
    const plan = prepareHybridTsdfPlan(input, false);
    expect(plan.mode).toBe('base-only');
    expect(plan.channelInputs).toHaveLength(0);
    expect(plan.baseMesh!.triangleCount).toBeGreaterThan(300);
    expect(plan.directionalFallbackPoints ?? 0).toBeLessThan(60);
  });

  it('a thin board measured from both sides (opposite normals cancel in a scalar field) is rescued by the directional pass and keeps both layers', () => {
    const positions: number[] = [], normals: number[] = [], confidence: number[] = [];
    for (const [z, n] of [[0.01, 1], [-0.01, -1]] as const) for (let a = 0; a <= 32; a += 1) for (let b = 0; b <= 32; b += 1) { positions.push(a * 0.05, b * 0.05, z); normals.push(0, 0, n); confidence.push(0.92); }
    const input = { positions: Float32Array.from(positions), normals: Float32Array.from(normals), confidence: Float32Array.from(confidence), voxelSizeMeters: 0.048, maxTriangles: 40000 };
    expect(buildSparseTsdfMesh(input).triangleCount, 'the scalar field alone loses the board').toBe(0);
    const mesh = buildDirectionalTsdfMesh(input);
    expect(mesh.triangleCount).toBeGreaterThan(1000);
    expect(mesh.directionalFallbackPoints).toBeGreaterThanOrEqual(60);
    const heights = new Set<number>();
    for (let i = 2; i < mesh.trianglePositions.length; i += 3) heights.add(Math.sign(mesh.trianglePositions[i]));
    expect(heights.has(1) && heights.has(-1), 'a layer on each side of the board').toBe(true);
  });

  it('describes the mesh as DISPLAYED: the reported components are those after the last geometry change', () => {
    const mesh = buildDirectionalTsdfMesh(planes([[0, 0, [1, 0, 0]], [1, 0, [0, 1, 0]]]));
    expect(mesh.finalTopology).toBeDefined();
    expect(mesh.meshTopologyComponentCount).toBe(mesh.finalTopology!.components);
    expect(mesh.finalTopology!.triangles).toBe(mesh.triangleCount);
    expect(mesh.finalTopology!.degenerateTriangles).toBe(0);
  });
});
