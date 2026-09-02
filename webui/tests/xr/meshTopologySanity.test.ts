import { describe, expect, it } from 'vitest';

import { sanitizeMeshTopology } from '../../src/xr/meshTopologySanity';

type Point = readonly [number, number, number];

function pushTriangle(output: number[], a: Point, b: Point, c: Point): void {
  output.push(...a, ...b, ...c);
}

function gridPatch(
  cellsX: number,
  cellsY: number,
  spacing = 0.04,
  origin: Point = [0, 0, 0],
  reverseAlternating = false,
): number[] {
  const output: number[] = [];
  let triangle = 0;
  for (let y = 0; y < cellsY; y += 1) {
    for (let x = 0; x < cellsX; x += 1) {
      const x0 = origin[0] + x * spacing;
      const x1 = x0 + spacing;
      const y0 = origin[1] + y * spacing;
      const y1 = y0 + spacing;
      const z = origin[2];
      const tris: readonly [Point, Point, Point][] = [
        [[x0, y0, z], [x1, y0, z], [x1, y1, z]],
        [[x0, y0, z], [x1, y1, z], [x0, y1, z]],
      ];
      for (const [a, b, c] of tris) {
        const reverse = reverseAlternating && (triangle & 1) === 1;
        pushTriangle(output, a, reverse ? c : b, reverse ? b : c);
        triangle += 1;
      }
    }
  }
  return output;
}

function verticalGridPatch(
  cellsX: number,
  cellsZ: number,
  spacing = 0.04,
  origin: Point = [0, 0, 0],
): number[] {
  const output: number[] = [];
  for (let z = 0; z < cellsZ; z += 1) {
    for (let x = 0; x < cellsX; x += 1) {
      const x0 = origin[0] + x * spacing;
      const x1 = x0 + spacing;
      const z0 = origin[2] + z * spacing;
      const z1 = z0 + spacing;
      const y = origin[1];
      pushTriangle(output, [x0, y, z0], [x1, y, z1], [x1, y, z0]);
      pushTriangle(output, [x0, y, z0], [x0, y, z1], [x1, y, z1]);
    }
  }
  return output;
}

function zNormalSign(positions: Float32Array, triangle: number): number {
  const offset = triangle * 9;
  const ax = positions[offset]; const ay = positions[offset + 1];
  const bx = positions[offset + 3]; const by = positions[offset + 4];
  const cx = positions[offset + 6]; const cy = positions[offset + 7];
  return Math.sign((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
}

describe('mesh topology sanity', () => {
  it('retains a measured 2-D patch and repairs inconsistent local winding', () => {
    const source = Float32Array.from(gridPatch(3, 2, 0.04, [0, 0, 0], true));
    const result = sanitizeMeshTopology(source, 0.04);
    expect(result.sourceTriangleCount).toBe(12);
    expect(result.retainedTriangleCount).toBe(12);
    expect(result.quarantinedTriangleCount).toBe(0);
    expect(result.discardedTriangleCount).toBe(0);
    expect(result.orientationRepairCount).toBeGreaterThan(0);
    expect(result.orientationConflictCount).toBe(0);
    expect(result.selfIntersectionPairCount).toBe(0);
    expect(result.localNoiseHotspotCount).toBe(0);
    expect(result.localNoiseHotspotTriangleCount).toBe(0);
    expect(result.components.every((component) => component.diagnosticsComplete)).toBe(true);
    const signs = Array.from({ length: result.triangleCount }, (_, index) => zNormalSign(result.trianglePositions, index));
    expect(new Set(signs).size).toBe(1);
  });

  it('does not render a tiny disconnected triangle as a physical mesh island', () => {
    const tiny = Float32Array.from([
      1, 1, 0,
      1.02, 1, 0,
      1, 1.02, 0,
    ]);
    const result = sanitizeMeshTopology(tiny, 0.04);
    expect(result.triangleCount).toBe(0);
    expect(result.discardedTriangleCount).toBe(1);
    expect(result.components[0].disposition).toBe('discard');
  });

  it('retains two separate substantial measured slabs rather than deleting one for being disconnected', () => {
    const source = Float32Array.from([
      ...gridPatch(3, 2, 0.04, [0, 0, 0]),
      ...gridPatch(3, 2, 0.04, [0.7, 0.3, 0.2]),
    ]);
    const result = sanitizeMeshTopology(source, 0.04);
    expect(result.components).toHaveLength(2);
    expect(result.components.every((component) => component.disposition === 'retain')).toBe(true);
    expect(result.retainedTriangleCount).toBe(24);
    expect(result.selfIntersectionPairCount).toBe(0);
  });

  it('detects a non-manifold edge and does not let that local defect poison a separate healthy component', () => {
    const healthy = gridPatch(3, 2, 0.04, [0, 0, 0]);
    const nonManifold: number[] = [];
    const a: Point = [1, 0, 0];
    const b: Point = [1.1, 0, 0];
    pushTriangle(nonManifold, a, b, [1.05, 0.08, 0]);
    pushTriangle(nonManifold, b, a, [1.05, -0.08, 0]);
    pushTriangle(nonManifold, a, b, [1.05, 0, 0.08]);

    const result = sanitizeMeshTopology(Float32Array.from([...healthy, ...nonManifold]), 0.04);
    expect(result.nonManifoldEdgeCount).toBeGreaterThanOrEqual(1);
    const healthyComponent = result.components.find((component) => component.triangleCount === 12);
    expect(healthyComponent?.disposition).toBe('retain');
    expect(result.retainedTriangleCount).toBe(12);
    expect(result.quarantinedTriangleCount + result.discardedTriangleCount).toBeGreaterThanOrEqual(3);
  });

  it('reports non-adjacent geometric crossings without using them as a standalone delete gate', () => {
    const source: number[] = [];
    pushTriangle(source, [-0.12, -0.10, 0], [0.12, -0.10, 0], [0, 0.14, 0]);
    pushTriangle(source, [0, -0.08, -0.12], [0, 0.12, 0.12], [0, 0.14, -0.12]);
    const result = sanitizeMeshTopology(Float32Array.from(source), 0.04);
    expect(result.selfIntersectionPairCount).toBe(1);
    expect(result.selfIntersectionAffectedTriangleCount).toBe(2);
    expect(result.components.some((component) => component.meshNoiseScore > 0)).toBe(true);
    // These two faces are too small to render for the ordinary component-size
    // reason; the test deliberately does not assert deletion *because* they intersect.
    expect(result.discardedTriangleCount).toBe(2);
  });

  it('keeps a welded 90-degree structural fold because adjacent faces are not self-intersections', () => {
    const source = Float32Array.from([
      ...gridPatch(3, 2, 0.04, [0, 0, 0]),
      ...verticalGridPatch(3, 2, 0.04, [0, 0, 0]),
    ]);
    const result = sanitizeMeshTopology(source, 0.04);
    expect(result.selfIntersectionPairCount).toBe(0);
    expect(result.components).toHaveLength(1);
    expect(result.components[0].meshNoiseScore).toBeLessThanOrEqual(0.60);
    expect(result.components[0].disposition).toBe('retain');
    expect(result.localNoiseHotspotCount).toBe(0);
    expect(result.localNoiseHotspotTriangleCount).toBe(0);
    expect(result.retainedTriangleCount).toBe(result.sourceTriangleCount);
  });

  it('quarantines a substantial crossing triangle soup when intersection noise is widespread', () => {
    const source = Float32Array.from([
      ...gridPatch(4, 3, 0.04, [0, 0, 0]),
      ...verticalGridPatch(4, 3, 0.04, [0.01, 0.055, -0.06]),
    ]);
    const result = sanitizeMeshTopology(source, 0.04);
    expect(result.selfIntersectionPairCount).toBeGreaterThan(0);
    const noisy = result.components.filter((component) => component.meshNoiseScore > 0.60);
    expect(noisy.length).toBeGreaterThan(0);
    expect(noisy.every((component) => component.disposition === 'quarantine')).toBe(true);
    expect(result.quarantinedTriangleCount).toBeGreaterThan(0);
  });

  it('locally certifies a healthy isolated slab when a far pathological island truncates the global diagnostic', () => {
    const source: number[] = [...gridPatch(3, 2, 0.04, [0, 0, 0])];
    // This giant disconnected face exceeds bounded diagnostic cell budgets, but
    // it is metres away from the healthy slab and cannot intersect it or be hit
    // by the short normal-intrusion ray. The healthy component may therefore earn
    // its own bounded local certificate instead of being globally quarantined.
    pushTriangle(source, [2, 0, 0], [5, 0, 0], [2, 3, 0]);

    const result = sanitizeMeshTopology(Float32Array.from(source), 0.04);
    expect(result.selfIntersectionTruncated).toBe(true);
    expect(result.localNoiseHotspotTriangleCount).toBe(0);
    const healthy = result.components.find((component) => component.triangleCount === 12);
    const pathological = result.components.find((component) => component.triangleCount === 1);
    expect(healthy?.diagnosticsComplete).toBe(true);
    expect(healthy?.disposition).toBe('retain');
    expect(pathological?.diagnosticsComplete).toBe(false);
    expect(pathological?.disposition).toBe('discard');
    expect(result.retainedTriangleCount).toBe(12);
  });

  it('does not locally certify a healthy-looking slab when an unverified giant island is close enough to interact', () => {
    const source: number[] = [...gridPatch(3, 2, 0.04, [0, 0, 0])];
    // Topologically disconnected, but only 3 cm above the slab. The giant face
    // makes the global pass incomplete and lies well inside the normal-intrusion
    // interaction margin, so a local-only recheck would miss cross-component
    // evidence and must not be treated as a complete certificate.
    pushTriangle(source, [-0.02, -0.02, 0.03], [3, -0.02, 0.03], [-0.02, 3, 0.03]);

    const result = sanitizeMeshTopology(Float32Array.from(source), 0.04);
    expect(result.selfIntersectionTruncated).toBe(true);
    expect(result.localNoiseHotspotTriangleCount).toBe(0);
    const healthy = result.components.find((component) => component.triangleCount === 12);
    expect(healthy?.diagnosticsComplete).toBe(false);
    expect(healthy?.disposition).not.toBe('retain');
    expect(result.retainedTriangleCount).toBe(0);
  });

  it('is deterministic for identical triangle soup', () => {
    const source = Float32Array.from(gridPatch(4, 3, 0.04, [0, 0, 0], true));
    const first = sanitizeMeshTopology(source, 0.04);
    const second = sanitizeMeshTopology(source, 0.04);
    expect(second.components).toEqual(first.components);
    expect(second.selfIntersectionPairCount).toBe(first.selfIntersectionPairCount);
    expect(second.selfIntersectionExactTestedPairs).toBe(first.selfIntersectionExactTestedPairs);
    expect(second.localNoiseHotspotCount).toBe(first.localNoiseHotspotCount);
    expect(second.localNoiseHotspotTriangleCount).toBe(first.localNoiseHotspotTriangleCount);
    expect(Array.from(second.trianglePositions)).toEqual(Array.from(first.trianglePositions));
  });
});
