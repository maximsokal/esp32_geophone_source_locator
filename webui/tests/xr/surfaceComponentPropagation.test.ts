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

function reverseCloud(source: Cloud): Cloud {
  const reversed = cloud();
  for (let index = source.confidence.length - 1; index >= 0; index -= 1) {
    const offset = index * 3;
    reversed.positions.push(source.positions[offset], source.positions[offset + 1], source.positions[offset + 2]);
    reversed.normals.push(source.normals[offset], source.normals[offset + 1], source.normals[offset + 2]);
    reversed.confidence.push(source.confidence[index]);
  }
  return reversed;
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

  it('keeps an extrema-only measured contact that index-quantile sampling can miss', () => {
    const source = cloud();
    const xs = [0.005, 0.010, 0.015, 0.020, 0.025, 0.035, 0.040, 0.045, 0.050, 0.055, 0.058];
    // Node A lives in z-cell 0. Its only support close to node B is max-Z at
    // x=2.2 cm, intentionally between ordinary evenly-spaced sample indices.
    for (const x of xs.slice(0, 4)) addPoint(source, x, normalAt(0), 0.001, 0.001);
    addPoint(source, 0.022, normalAt(0), 0.030, 0.059);
    for (const x of xs.slice(4)) addPoint(source, x, normalAt(0), 0.001, 0.001);
    // Node B lives in the adjacent z-cell. The matching min-Z support is likewise
    // interior in lexicographic x order; all other A/B pairs are >7 cm apart.
    for (const x of xs.slice(0, 4)) addPoint(source, x, normalAt(0), 0.059, 0.119);
    addPoint(source, 0.022, normalAt(0), 0.030, 0.061);
    for (const x of xs.slice(4)) addPoint(source, x, normalAt(0), 0.059, 0.119);

    const forward = analyze(source);
    const reversed = analyze(reverseCloud(source));
    expect(forward.acceptedPointCount).toBe(24);
    expect(forward.componentCount).toBe(1);
    expect(forward.components[0].pointCount).toBe(24);
    expect(reversed.componentCount).toBe(1);
    expect(reversed.components[0].pointCount).toBe(24);
  });
});