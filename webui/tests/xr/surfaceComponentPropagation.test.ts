import { describe, expect, it } from 'vitest';

import { analyzeSurfaceComponents } from '../../src/xr/surfaceComponents';

interface Cloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): Cloud { return { positions: [], normals: [], confidence: [] }; }

function normalAt(degrees: number): readonly [number, number, number] {
  const radians = degrees * Math.PI / 180;
  return [Math.sin(radians), 0, Math.cos(radians)];
}

function addPoint(
  target: Cloud,
  x: number,
  normal: readonly [number, number, number],
  y = 0.012,
  z = 0.012,
): void {
  target.positions.push(x, y, z);
  target.normals.push(...normal);
  target.confidence.push(0.92);
}

function addDenseNode(
  target: Cloud,
  x: number,
  normal: readonly [number, number, number],
): void {
  for (const [y, z] of [[0.008, 0.008], [0.016, 0.008], [0.008, 0.016], [0.016, 0.016]] as const) {
    addPoint(target, x, normal, y, z);
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

describe('surface component propagation support', () => {
  it('lets a sparse measured node attach as a leaf to a dense surface', () => {
    const source = cloud();
    addDenseNode(source, 0.055, normalAt(0));
    addPoint(source, 0.065, normalAt(20));

    const result = analyze(source);
    expect(result.componentCount).toBe(1);
    expect(result.components[0].pointCount).toBe(5);
  });

  it('does not let a sparse gradual-normal chain bridge two dense surfaces', () => {
    const source = cloud();
    addDenseNode(source, 0.055, normalAt(0));
    addPoint(source, 0.065, normalAt(20));
    addPoint(source, 0.125, normalAt(40));
    addPoint(source, 0.185, normalAt(60));
    addDenseNode(source, 0.245, normalAt(80));

    const result = analyze(source);
    const sizes = result.components.map((component) => component.pointCount).sort((a, b) => a - b);
    expect(result.acceptedPointCount).toBe(11);
    expect(result.componentCount).toBe(3);
    expect(sizes).toEqual([1, 5, 5]);
  });

  it('keeps a physically supported dense smooth convex normal chain connected', () => {
    const source = cloud();
    addDenseNode(source, 0.055, normalAt(0));
    addDenseNode(source, 0.065, normalAt(20));
    addDenseNode(source, 0.125, normalAt(40));
    addDenseNode(source, 0.185, normalAt(60));
    addDenseNode(source, 0.245, normalAt(80));

    const result = analyze(source);
    expect(result.acceptedPointCount).toBe(20);
    expect(result.componentCount).toBe(1);
    expect(result.components[0].pointCount).toBe(20);
  });
});