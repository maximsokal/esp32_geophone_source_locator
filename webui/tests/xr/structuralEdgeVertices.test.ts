import { describe, expect, it } from 'vitest';

import { inferStructuralVerticesFromEdges } from '../../src/xr/structuralEdgeVertices';
import type { StructuralEdgeCandidate } from '../../src/xr/structuralGuide';

function edge(
  id: string,
  start: readonly [number, number, number],
  end: readonly [number, number, number],
  overrides: Partial<StructuralEdgeCandidate> = {},
): StructuralEdgeCandidate {
  const dx = end[0] - start[0], dy = end[1] - start[1], dz = end[2] - start[2];
  const length = Math.hypot(dx, dy, dz) || 1;
  return {
    id,
    kind: 'edge',
    start,
    end,
    midpoint: [(start[0] + end[0]) * 0.5, (start[1] + end[1]) * 0.5, (start[2] + end[2]) * 0.5],
    direction: [dx / length, dy / length, dz / length],
    confidence: 0.84,
    geometryConfidence: 0.80,
    support: 24,
    observations: 5,
    ...overrides,
  };
}

describe('structural vertices from mature finite edges', () => {
  it('infers a corner where two mature perpendicular edge endpoints meet', () => {
    const vertices = inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.5, 0, 0]),
      edge('b', [0.51, 0.01, 0], [0.51, 0.45, 0]),
    ]);
    expect(vertices).toHaveLength(1);
    expect(vertices[0].id).toContain('edge-vertex:');
    expect(vertices[0].position[0]).toBeCloseTo(0.505, 2);
    expect(vertices[0].position[1]).toBeCloseTo(0.005, 2);
    expect(vertices[0].observations).toBe(5);
    expect(vertices[0].confidence).toBeGreaterThanOrEqual(0.70);
  });

  it('does not invent a vertex from parallel fragmented edges', () => {
    expect(inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.5, 0, 0]),
      edge('b', [0.51, 0.02, 0], [0.9, 0.02, 0]),
    ])).toHaveLength(0);
  });

  it('does not bridge distant endpoints', () => {
    expect(inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.5, 0, 0]),
      edge('b', [0.65, 0, 0], [0.65, 0.4, 0]),
    ])).toHaveLength(0);
  });

  it('does not let two minimum-scale edges bridge a five-centimetre hole', () => {
    expect(inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.10, 0, 0], { support: 8 }),
      edge('b', [0.15, 0, 0], [0.15, 0.10, 0], { support: 8 }),
    ])).toHaveLength(0);
  });

  it('allows a bounded sensor hole only when both measured edges are long and dense', () => {
    const vertices = inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.50, 0, 0], { support: 24 }),
      edge('b', [0.55, 0, 0], [0.55, 0.50, 0], { support: 24 }),
    ]);
    expect(vertices).toHaveLength(1);
    // The 5-cm hole is allowed for long mature edges, but the inferred vertex
    // remains only half that distance from each measured finite endpoint.
    expect(vertices[0].position[0]).toBeCloseTo(0.525, 3);
  });

  it('requires independent maturity on both contributing edges', () => {
    expect(inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.5, 0, 0]),
      edge('b', [0.51, 0.01, 0], [0.51, 0.45, 0], { observations: 2 }),
    ])).toHaveLength(0);
  });

  it('requires the operational minimum physical edge length before corner inference', () => {
    expect(inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.08, 0, 0]),
      edge('b', [0.08, 0, 0], [0.08, 0.08, 0]),
    ])).toHaveLength(0);
  });

  it('does not let long but under-supported lines create a vertex', () => {
    expect(inferStructuralVerticesFromEdges([
      edge('a', [0, 0, 0], [0.5, 0, 0], { support: 8 }),
      edge('b', [0.5, 0, 0], [0.5, 0.5, 0], { support: 8 }),
    ])).toHaveLength(0);
  });

  it('deduplicates three mature edges meeting at one physical vertex', () => {
    const vertices = inferStructuralVerticesFromEdges([
      edge('x', [0, 0, 0], [0.5, 0, 0]),
      edge('y', [0.49, 0.01, 0], [0.49, 0.5, 0]),
      edge('z', [0.50, 0.00, 0.01], [0.50, 0.00, 0.5]),
    ]);
    expect(vertices).toHaveLength(1);
    expect(vertices[0].support).toBeGreaterThanOrEqual(48);
  });
});
