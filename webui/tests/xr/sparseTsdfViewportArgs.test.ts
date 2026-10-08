import { describe, expect, it } from 'vitest';

import { buildTsdfRequestArgs } from '../../src/components/ScanPointCloudViewport';
import {
  prepareDirectionalTsdfPlan,
} from '../../src/xr/directionalTsdf';
import { buildSparseTsdfMesh } from '../../src/xr/sparseTsdf';

function planeSnapshot(occupancyVoxelSizeMeters: number, occupancyFineVoxelSizeMeters?: number) {
  const positions: number[] = [];
  const normals: number[] = [];
  for (let y = -4; y <= 4; y += 1) {
    for (let z = -4; z <= 4; z += 1) {
      positions.push(0, y * 0.04, z * 0.04);
      normals.push(-1, 0, 0);
    }
  }
  return {
    tsdfPointCount: 81,
    tsdfPositions: Float32Array.from(positions),
    tsdfNormals: Float32Array.from(normals),
    tsdfCoverageConfidence: new Float32Array(81).fill(0.94),
    voxelSizeMeters: 0.04,
    freeAirPositions: Float32Array.from([0.6, 0, 0]),
    occupancyVoxelSizeMeters,
    ...(occupancyFineVoxelSizeMeters === undefined ? {} : { occupancyFineVoxelSizeMeters }),
  };
}

function largePlaneSnapshot(occupancyVoxelSizeMeters: number, occupancyFineVoxelSizeMeters: number) {
  const positions = new Float32Array(10_100 * 3);
  const normals = new Float32Array(10_100 * 3);
  for (let index = 0; index < 10_100; index += 1) {
    const y = (index % 101) - 50;
    const z = Math.floor(index / 101) - 49;
    const offset = index * 3;
    positions[offset] = 0;
    positions[offset + 1] = y * 0.04;
    positions[offset + 2] = z * 0.04;
    normals[offset] = -1;
  }
  return {
    tsdfPointCount: 10_100,
    tsdfPositions: positions,
    tsdfNormals: normals,
    tsdfCoverageConfidence: new Float32Array(10_100).fill(0.94),
    voxelSizeMeters: 0.04,
    freeAirPositions: Float32Array.from([0.6, 0, 0]),
    occupancyVoxelSizeMeters,
    occupancyFineVoxelSizeMeters,
  };
}

function allNormalsPointNegativeX(normals: Float32Array): boolean {
  for (let offset = 0; offset < normals.length; offset += 3) {
    if (normals[offset] > -0.99 || Math.abs(normals[offset + 1]) > 1e-6 || Math.abs(normals[offset + 2]) > 1e-6) return false;
  }
  return true;
}

describe('post-scan TSDF viewport request scales', () => {
  it('uses fine FREE spacing through the large-envelope scalar and directional paths', () => {
    const snapshot = planeSnapshot(0.4, 0.1);
    const request = buildTsdfRequestArgs(snapshot);
    expect(request.freeSpaceVoxelSizeMeters).toBe(0.1);

    const finePlan = prepareDirectionalTsdfPlan(request, true);
    const coarsePlan = prepareDirectionalTsdfPlan({
      ...request,
      freeSpaceVoxelSizeMeters: snapshot.occupancyVoxelSizeMeters,
    }, true);
    expect(finePlan.mode).toBe('single');
    expect(coarsePlan.mode).toBe('single');
    expect(allNormalsPointNegativeX(finePlan.channelInputs[0].normals)).toBe(true);
    expect(coarsePlan.channelInputs[0].normals[0]).toBeGreaterThan(0.99);

    const scalarFine = buildSparseTsdfMesh(request);
    const scalarDirectionalFine = buildSparseTsdfMesh(finePlan.channelInputs[0]);
    const scalarCoarse = buildSparseTsdfMesh({
      ...request,
      freeSpaceVoxelSizeMeters: snapshot.occupancyVoxelSizeMeters,
    });
    expect(Array.from(scalarDirectionalFine.trianglePositions)).toEqual(Array.from(scalarFine.trianglePositions));
    expect(Array.from(scalarCoarse.trianglePositions)).not.toEqual(Array.from(scalarFine.trianglePositions));
  });

  it('keeps the no-coarsening scalar and directional control equivalent', () => {
    const request = buildTsdfRequestArgs(planeSnapshot(0.1, 0.1));
    const plan = prepareDirectionalTsdfPlan(request, true);
    const scalar = buildSparseTsdfMesh(request);
    const directionalChannelMesh = buildSparseTsdfMesh(plan.channelInputs[0]);
    expect(plan.mode).toBe('single');
    expect(allNormalsPointNegativeX(plan.channelInputs[0].normals)).toBe(true);
    expect(Array.from(directionalChannelMesh.trianglePositions)).toEqual(Array.from(scalar.trianglePositions));
    expect(directionalChannelMesh.triangleCount).toBe(scalar.triangleCount);
  });

  it('keeps legacy and no-coarsening requests geometry-equivalent', () => {
    const fineSnapshot = planeSnapshot(0.1, 0.1);
    const legacySnapshot = planeSnapshot(0.1);
    const fineRequest = buildTsdfRequestArgs(fineSnapshot);
    const legacyRequest = buildTsdfRequestArgs(legacySnapshot);
    expect(fineRequest.freeSpaceVoxelSizeMeters).toBe(0.1);
    expect(legacyRequest.freeSpaceVoxelSizeMeters).toBe(0.1);

    const fineMesh = buildSparseTsdfMesh(fineRequest);
    const legacyMesh = buildSparseTsdfMesh(legacyRequest);
    expect(Array.from(legacyMesh.trianglePositions)).toEqual(Array.from(fineMesh.trianglePositions));
    expect(legacyMesh.triangleCount).toBe(fineMesh.triangleCount);
  });

  it('keeps fine FREE orientation through buildTsdfRequestArgs when input stride exceeds one', () => {
    const snapshot = largePlaneSnapshot(0.4, 0.1);
    const request = buildTsdfRequestArgs(snapshot);
    expect(request.freeSpaceVoxelSizeMeters).toBe(0.1);
    expect(request.maxInputPoints).toBe(9000);

    const actual = buildSparseTsdfMesh(request);
    const trueFineReference = buildSparseTsdfMesh({
      ...request,
      freeSpaceVoxelSizeMeters: snapshot.occupancyFineVoxelSizeMeters,
    });
    expect(actual.integratedPointCount).toBeLessThan(actual.sourcePointCount);
    expect(Array.from(actual.trianglePositions)).toEqual(Array.from(trueFineReference.trianglePositions));
    expect(actual.triangleCount).toBe(trueFineReference.triangleCount);
  });
});
