import { describe, expect, it } from 'vitest';

import { assessMeshReconstructionPreflight } from '../../src/xr/meshReconstructionPreflight';

interface Cloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): Cloud { return { positions: [], normals: [], confidence: [] }; }

function addGrid(target: Cloud, originX: number, width: number, height: number, spacing: number): void {
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      target.positions.push(originX + x * spacing, y * spacing, 0);
      target.normals.push(0, 0, 1);
      target.confidence.push(0.92);
    }
  }
}

function assess(source: Cloud) {
  return assessMeshReconstructionPreflight({
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    voxelSizeMeters: 0.04,
  });
}

describe('mesh reconstruction preflight', () => {
  it('blocks an expensive reconstruction for a dense but tiny patch', () => {
    const source = cloud();
    addGrid(source, 0, 10, 10, 0.006);
    const result = assess(source);
    expect(result.sourcePointCount).toBe(100);
    expect(result.ready).toBe(false);
    expect(result.meshReadiness).toBeLessThan(1);
    expect(result.limitingReason).toBe('extent-or-density');
  });

  it('blocks a long but physically narrow strip that is not a meaningful 2-D mesh region', () => {
    const source = cloud();
    addGrid(source, 0, 12, 2, 0.04);
    const result = assess(source);
    expect(result.sourcePointCount).toBe(24);
    expect(result.ready).toBe(false);
    expect(result.eligibleComponentCount).toBe(0);
    expect(result.eligiblePointCount).toBe(0);
    expect(result.meshReadiness).toBeLessThan(1);
    expect(result.limitingReason).toBe('extent-or-density');
  });

  it('allows a genuinely two-dimensional connected surface', () => {
    const source = cloud();
    addGrid(source, 0, 9, 9, 0.04);
    const result = assess(source);
    expect(result.ready).toBe(true);
    expect(result.eligibleComponentCount).toBe(1);
    expect(result.eligiblePointCount).toBe(81);
    expect(result.meshReadiness).toBe(1);
  });

  it('does not add disconnected undersized islands together to cross the point threshold', () => {
    const source = cloud();
    addGrid(source, -0.6, 6, 6, 0.01);
    addGrid(source, 0.6, 6, 6, 0.01);
    const result = assess(source);
    expect(result.sourcePointCount).toBe(72);
    expect(result.componentCount).toBe(2);
    expect(result.ready).toBe(false);
    expect(result.eligiblePointCount).toBe(0);
  });

  it('reports a point-count limitation before scale can matter', () => {
    const source = cloud();
    addGrid(source, 0, 4, 4, 0.05);
    const result = assess(source);
    expect(result.ready).toBe(false);
    expect(result.limitingReason).toBe('points');
  });
});
