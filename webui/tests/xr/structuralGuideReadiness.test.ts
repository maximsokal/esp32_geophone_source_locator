import { describe, expect, it } from 'vitest';

import type { StructuralGuidePointCloudSnapshot } from '../../src/xr/depthPointCloudV2';
import { prepareStructuralGuideReadiness } from '../../src/xr/structuralGuideReadiness';
import { MESH_MIN_POINTS } from '../../src/xr/surfaceComponents';

function flatMeasuredGrid(size = 10, spacing = 0.04): StructuralGuidePointCloudSnapshot {
  const positions: number[] = [];
  const normals: number[] = [];
  const confidence: number[] = [];
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      positions.push(x * spacing, y * spacing, 0);
      normals.push(0, 0, 1);
      confidence.push(0.94);
    }
  }
  return {
    positions: Float32Array.from(positions),
    normals: Float32Array.from(normals),
    confidence: Float32Array.from(confidence),
    pointCount: confidence.length,
    keyframeCount: 8,
    voxelSizeMeters: 0.04,
  };
}

describe('separate structural and mesh readiness preparation', () => {
  it('uses family-clustered certified surfels for S and unclustered certified physical surfels for M', () => {
    const raw = flatMeasuredGrid();
    const prepared = prepareStructuralGuideReadiness(raw);

    expect(prepared.rawPointCount).toBe(100);
    expect(prepared.meshSurface.pointCount).toBeGreaterThanOrEqual(MESH_MIN_POINTS);
    expect(prepared.clusteredSurface.pointCount).toBeLessThan(prepared.meshSurface.pointCount);
    expect(prepared.structuralSurface.pointCount).toBeLessThanOrEqual(prepared.clusteredSurface.pointCount);

    expect(prepared.surfaceReadiness.bestMeshComponent).not.toBeNull();
    expect(prepared.surfaceReadiness.bestMeshComponent!.meshEligible).toBe(true);
    expect(prepared.surfaceReadiness.bestMeshComponent!.pointCount).toBe(prepared.meshSurface.pointCount);

    expect(prepared.surfaceReadiness.structuralComponent).not.toBeNull();
    expect(prepared.surfaceReadiness.structuralComponent!.pointCount).toBeGreaterThan(0);
    expect(prepared.surfaceReadiness.structuralComponent!.pointCount)
      .toBeLessThanOrEqual(prepared.structuralSurface.pointCount);
    expect(prepared.surfaceReadiness.structuralComponent!.pointCount)
      .toBeLessThan(prepared.surfaceReadiness.bestMeshComponent!.pointCount);
  });

  it('does not let the structural family reduction make a physically mesh-ready slab report M below 100%', () => {
    const prepared = prepareStructuralGuideReadiness(flatMeasuredGrid());
    expect(prepared.surfaceReadiness.bestMeshComponent?.meshReadiness).toBe(1);
    expect(prepared.surfaceReadiness.bestMeshComponent?.meshEligible).toBe(true);
  });
});
