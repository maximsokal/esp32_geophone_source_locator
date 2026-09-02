import { describe, expect, it } from 'vitest';

import { analyzeSurfaceComponents } from '../../src/xr/surfaceComponents';

interface Cloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): Cloud { return { positions: [], normals: [], confidence: [] }; }

function addGrid(
  target: Cloud,
  origin: readonly [number, number, number],
  width: number,
  height: number,
  spacing: number,
  normal: readonly [number, number, number] = [0, 0, 1],
): void {
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      target.positions.push(origin[0] + x * spacing, origin[1] + y * spacing, origin[2]);
      target.normals.push(...normal);
      target.confidence.push(0.92);
    }
  }
}

function analyze(source: Cloud) {
  return analyzeSurfaceComponents({
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    voxelSizeMeters: 0.04,
  });
}

describe('bounded spatial surface components', () => {
  it('rejects a dense but physically tiny patch from mesh eligibility', () => {
    const source = cloud();
    addGrid(source, [0, 0, 0], 7, 7, 0.01);
    const result = analyze(source);
    expect(result.acceptedPointCount).toBe(49);
    expect(result.componentCount).toBe(1);
    expect(result.bestMeshComponent).not.toBeNull();
    expect(result.bestMeshComponent!.majorExtentMeters).toBeLessThan(0.10);
    expect(result.bestMeshComponent!.meshReadiness).toBeLessThan(1);
    expect(result.eligiblePointCount).toBe(0);
  });

  it('accepts a sufficiently large, dense two-dimensional measured surface', () => {
    const source = cloud();
    addGrid(source, [0, 0, 0], 9, 9, 0.04);
    const result = analyze(source);
    expect(result.componentCount).toBe(1);
    expect(result.eligibleComponentCount).toBe(1);
    expect(result.eligiblePointCount).toBe(81);
    expect(result.bestMeshComponent!.majorExtentMeters).toBeGreaterThan(0.20);
    expect(result.bestMeshComponent!.minorExtentMeters).toBeGreaterThan(0.20);
    expect(result.bestMeshComponent!.meshReadiness).toBe(1);
  });

  it('does not combine disconnected tiny islands merely to satisfy a global point count', () => {
    const source = cloud();
    addGrid(source, [-0.5, 0, 0], 6, 6, 0.012);
    addGrid(source, [0.5, 0, 0], 6, 6, 0.012);
    const result = analyze(source);
    expect(result.acceptedPointCount).toBe(72);
    expect(result.componentCount).toBe(2);
    expect(result.eligibleComponentCount).toBe(0);
    expect(result.eligiblePointCount).toBe(0);
  });

  it('keeps orthogonal surface families disconnected even when they share spatial cells', () => {
    const source = cloud();
    addGrid(source, [0, 0, 0], 8, 8, 0.04, [0, 0, 1]);
    for (let z = 0; z < 8; z += 1) {
      for (let x = 0; x < 8; x += 1) {
        source.positions.push(x * 0.04, 0, z * 0.04);
        source.normals.push(0, 1, 0);
        source.confidence.push(0.92);
      }
    }
    const result = analyze(source);
    expect(result.componentCount).toBeGreaterThanOrEqual(2);
    expect(result.eligibleComponentCount).toBeGreaterThanOrEqual(2);
  });

  it('respects a coverage-ready mask before component growth', () => {
    const source = cloud();
    addGrid(source, [0, 0, 0], 9, 9, 0.04);
    const ready = new Uint8Array(source.confidence.length);
    ready.fill(1, 0, 12);
    const result = analyzeSurfaceComponents({
      positions: Float32Array.from(source.positions),
      normals: Float32Array.from(source.normals),
      confidence: Float32Array.from(source.confidence),
      readyMask: ready,
      voxelSizeMeters: 0.04,
    });
    expect(result.acceptedPointCount).toBe(12);
    expect(result.eligibleComponentCount).toBe(0);
  });
});
