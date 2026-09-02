import { describe, expect, it } from 'vitest';

import type { StructuralGuidePointCloudSnapshot } from '../../src/xr/depthPointCloudV2';
import { filterStructuralSurface } from '../../src/xr/structuralSurfacePreprocess';

function flatGrid(size = 8, spacing = 0.035): StructuralGuidePointCloudSnapshot {
  const positions: number[] = [];
  const normals: number[] = [];
  const confidence: number[] = [];
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      positions.push(x * spacing, y * spacing, 0);
      normals.push(0, 0, 1);
      confidence.push(0.92);
    }
  }
  return {
    positions: Float32Array.from(positions),
    normals: Float32Array.from(normals),
    confidence: Float32Array.from(confidence),
    pointCount: confidence.length,
    keyframeCount: 12,
    voxelSizeMeters: 0.04,
  };
}

describe('structural surface preprocessing', () => {
  it('certifies locally supported physical surfels and attaches bounded quality evidence', () => {
    const source = flatGrid();
    const filtered = filterStructuralSurface(source);

    expect(filtered.pointCount).toBeGreaterThan(24);
    expect(filtered.pointCount).toBeLessThanOrEqual(source.pointCount);
    expect(filtered.residualMeters).toBeDefined();
    expect(filtered.normalConfidence).toBeDefined();
    expect(filtered.coverageConfidence).toBeDefined();
    expect(filtered.coverageReady).toBeDefined();
    expect(filtered.residualMeters).toHaveLength(filtered.pointCount);
    expect(filtered.normalConfidence).toHaveLength(filtered.pointCount);
    expect(filtered.coverageConfidence).toHaveLength(filtered.pointCount);
    expect(filtered.coverageReady).toHaveLength(filtered.pointCount);
    expect(Array.from(filtered.coverageReady!).every((value) => value === 1)).toBe(true);
  });

  it('fails open to measured geometry when local support is too sparse instead of fabricating or deleting the cloud', () => {
    const positions: number[] = [];
    const normals: number[] = [];
    const confidence: number[] = [];
    for (let index = 0; index < 30; index += 1) {
      positions.push(index * 0.30, 0, 0);
      normals.push(0, 0, 1);
      confidence.push(0.92);
    }
    const source: StructuralGuidePointCloudSnapshot = {
      positions: Float32Array.from(positions),
      normals: Float32Array.from(normals),
      confidence: Float32Array.from(confidence),
      pointCount: confidence.length,
      keyframeCount: 10,
      voxelSizeMeters: 0.04,
    };

    const filtered = filterStructuralSurface(source);
    expect(filtered).toBe(source);
    expect(filtered.pointCount).toBe(30);
    expect(Array.from(filtered.positions)).toEqual(Array.from(source.positions));
  });

  it('does not preserve one incompatible normal outlier as a certified structural surfel', () => {
    const source = flatGrid();
    const center = Math.floor(source.pointCount / 2);
    source.normals[center * 3] = 1;
    source.normals[center * 3 + 1] = 0;
    source.normals[center * 3 + 2] = 0;

    const filtered = filterStructuralSurface(source);
    expect(filtered).not.toBe(source);
    expect(filtered.pointCount).toBeLessThan(source.pointCount);
    for (let index = 0; index < filtered.pointCount; index += 1) {
      const offset = index * 3;
      expect(Math.abs(filtered.normals[offset + 2])).toBeGreaterThan(0.9);
    }
  });
});
