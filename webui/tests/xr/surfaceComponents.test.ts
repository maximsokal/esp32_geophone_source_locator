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

function addCluster(
  target: Cloud,
  x: number,
  normal: readonly [number, number, number] = [0, 0, 1],
): void {
  for (const [dy, dz] of [[0, 0], [0.008, 0], [0, 0.008], [0.008, 0.008]] as const) {
    target.positions.push(x, 0.010 + dy, 0.010 + dz);
    target.normals.push(...normal);
    target.confidence.push(0.92);
  }
}

function addFamily(
  target: Cloud,
  count: number,
  normal: readonly [number, number, number],
  offset: number,
): void {
  for (let index = 0; index < count; index += 1) {
    target.positions.push(
      0.004 + ((index + offset) % 3) * 0.006,
      0.004 + (Math.floor((index + offset) / 3) % 3) * 0.006,
      0.004 + (Math.floor((index + offset) / 9) % 2) * 0.006,
    );
    target.normals.push(...normal);
    target.confidence.push(0.92);
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

  it('does not bridge same-normal supports merely because their hash cells are adjacent', () => {
    const source = cloud();
    // With voxel=4 cm the connection cell is 6 cm. x=1 mm and x=119 mm land
    // in cells 0 and 1, so the old cell-only BFS incorrectly joined them even
    // though almost 12 cm of unmeasured space separates their actual supports.
    addCluster(source, 0.001);
    addCluster(source, 0.119);
    const result = analyze(source);
    expect(result.connectionCellSizeMeters).toBeCloseTo(0.06);
    expect(result.acceptedPointCount).toBe(8);
    expect(result.componentCount).toBe(2);
    expect(result.components.map((component) => component.pointCount).sort((a, b) => a - b)).toEqual([4, 4]);
  });

  it('still connects physically close support across a hash-cell boundary', () => {
    const source = cloud();
    addCluster(source, 0.055);
    addCluster(source, 0.065);
    const result = analyze(source);
    expect(result.acceptedPointCount).toBe(8);
    expect(result.componentCount).toBe(1);
    expect(result.components[0].pointCount).toBe(8);
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

  it('keeps the three strongest normal families in a crowded voxel independent of arrival order', () => {
    const first = cloud();
    addFamily(first, 10, [1, 0, 0], 0);
    addFamily(first, 8, [0, 1, 0], 20);
    addFamily(first, 6, [0, 0, 1], 40);
    addFamily(first, 4, [0.577350269, 0.577350269, 0.577350269], 60);

    const reversed = cloud();
    addFamily(reversed, 4, [0.577350269, 0.577350269, 0.577350269], 60);
    addFamily(reversed, 6, [0, 0, 1], 40);
    addFamily(reversed, 8, [0, 1, 0], 20);
    addFamily(reversed, 10, [1, 0, 0], 0);

    const a = analyze(first);
    const b = analyze(reversed);
    expect(a.acceptedPointCount).toBe(24);
    expect(b.acceptedPointCount).toBe(24);
    expect(a.components.map((component) => component.pointCount).sort((x, y) => y - x)).toEqual([10, 8, 6]);
    expect(b.components.map((component) => component.pointCount).sort((x, y) => y - x)).toEqual([10, 8, 6]);
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

  it('does not let a sparse connected tail inflate a tiny patch into a mesh-sized component', () => {
    const source = cloud();
    addGrid(source, [0, 0, 0], 7, 7, 0.01);
    // Four high-confidence points form a genuinely connected tail at 5-cm steps.
    // The metric gate keeps it connected, while robust trimmed PCA extent ensures
    // this sparse tail still cannot redefine the object's meaningful mesh size.
    for (const x of [0.10, 0.15, 0.20, 0.25]) {
      source.positions.push(x, 0.03, 0);
      source.normals.push(0, 0, 1);
      source.confidence.push(0.92);
    }
    const result = analyze(source);
    expect(result.componentCount).toBe(1);
    expect(result.bestMeshComponent!.pointCount).toBe(53);
    expect(result.bestMeshComponent!.majorExtentMeters).toBeLessThan(0.10);
    expect(result.bestMeshComponent!.meshEligible).toBe(false);
    expect(result.eligiblePointCount).toBe(0);
  });
});
