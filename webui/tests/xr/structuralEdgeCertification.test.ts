import { describe, expect, it } from 'vitest';

import {
  certifyOperationalStructuralEdges,
  mergeConnectedStructuralEdges,
} from '../../src/xr/structuralEdgeCertification';
import type { StructuralEdgeCandidate } from '../../src/xr/structuralGuide';
import type { VisualEvidenceFrame } from '../../src/xr/visualLineEvidence';

const projection = new Float32Array([
  1,0,0,0,
  0,1,0,0,
  0,0,0,-1,
  0,0,0,0,
]);

function edge(overrides: Partial<StructuralEdgeCandidate> = {}): StructuralEdgeCandidate {
  return {
    id: 'hardware:mature-edge',
    kind: 'edge',
    start: [-0.5, 2, 0],
    end: [0.5, 2, 0],
    midpoint: [0, 2, 0],
    direction: [1, 0, 0],
    confidence: 0.871,
    geometryConfidence: 0.802,
    support: 32,
    observations: 7,
    ...overrides,
  };
}

function fragment(
  id: string,
  startX: number,
  endX: number,
  y = 2,
  support = 8,
): StructuralEdgeCandidate {
  return edge({
    id,
    start: [startX, y, 0],
    end: [endX, y, 0],
    midpoint: [(startX + endX) * 0.5, y, 0],
    support,
    observations: 3,
    confidence: 0.79,
    geometryConfidence: 0.76,
  });
}

function projectedU(projectX: number, cameraX: number): number {
  return 0.5 + (projectX - cameraX) / 4;
}

function visuallyInconsistentFrame(keyframeId: number, cameraX: number): VisualEvidenceFrame {
  const left = projectedU(-0.5, cameraX);
  const right = projectedU(0.5, cameraX);
  return {
    keyframeId,
    capturedAtMs: keyframeId * 100,
    projectionMatrix: projection.slice(),
    worldFromView: new Float32Array([
      1,0,0,0,
      0,1,0,0,
      0,0,1,0,
      cameraX,0,0,1,
    ]),
    lines: [{
      id: `shifted:${keyframeId}`,
      start: [left, 0.575],
      end: [right, 0.575],
      direction: [1, 0],
      length: right - left,
      strength: 0.9,
      supportPixels: 40,
    }],
    junctions: [],
  };
}

describe('connected structural edge assembly', () => {
  it('stitches adjacent mature local fragments into their measured physical line span', () => {
    const merged = mergeConnectedStructuralEdges([
      fragment('edge:e1', 0.00, 0.12),
      fragment('edge:e2', 0.10, 0.23),
      fragment('edge:e3', 0.21, 0.35),
    ]);
    expect(merged).toHaveLength(1);
    expect(merged[0].start[0]).toBeCloseTo(0, 2);
    expect(merged[0].end[0]).toBeCloseTo(0.35, 2);
    expect(merged[0].support).toBeGreaterThanOrEqual(16);
    // Whole-line maturity may never exceed the weakest physical fragment.
    expect(merged[0].observations).toBe(3);
  });

  it('does not join nearby parallel edges that are laterally distinct', () => {
    const merged = mergeConnectedStructuralEdges([
      fragment('edge:e1', 0.00, 0.20, 2.00),
      fragment('edge:e2', 0.02, 0.22, 2.08),
    ]);
    expect(merged).toHaveLength(2);
  });

  it('does not join collinear fragments across an unmeasured axial gap', () => {
    const merged = mergeConnectedStructuralEdges([
      fragment('edge:e1', 0.00, 0.12),
      fragment('edge:e2', 0.24, 0.36),
    ]);
    expect(merged).toHaveLength(2);
  });

  it('is deterministic under input permutation', () => {
    const source = [
      fragment('edge:e1', 0.00, 0.12),
      fragment('edge:e2', 0.10, 0.23),
      fragment('edge:e3', 0.21, 0.35),
    ];
    const first = mergeConnectedStructuralEdges(source);
    const second = mergeConnectedStructuralEdges([source[2], source[0], source[1]]);
    expect(second).toEqual(first);
  });
});

describe('operational edge to certified registry gate', () => {
  it('passes the mature edge observed on the phone even when simple Hough gives a conclusive miss', () => {
    const frames = [
      visuallyInconsistentFrame(1, 0),
      visuallyInconsistentFrame(2, 0.10),
      visuallyInconsistentFrame(3, 0.20),
    ];
    const certified = certifyOperationalStructuralEdges([edge()], frames);
    expect(certified).toHaveLength(1);
    expect(certified[0].confidence).toBeGreaterThanOrEqual(0.871);
    expect(certified[0].observations).toBe(7);
    expect(certified[0].geometryConfidence).toBeCloseTo(0.802, 3);
  });

  it('still rejects a physically immature edge regardless of RGB texture', () => {
    expect(certifyOperationalStructuralEdges([edge({ observations: 1 })], [])).toHaveLength(0);
    expect(certifyOperationalStructuralEdges([edge({ confidence: 0.62 })], [])).toHaveLength(0);
  });

  it('rejects a tiny finite segment that cannot be a useful structural constraint', () => {
    expect(certifyOperationalStructuralEdges([edge({
      start: [0, 2, 0],
      end: [0.03, 2, 0],
      midpoint: [0.015, 2, 0],
    })], [])).toHaveLength(0);
  });

  it('requires point support to grow with the physical length of the certified line', () => {
    expect(certifyOperationalStructuralEdges([edge({
      start: [-0.75, 2, 0],
      end: [0.75, 2, 0],
      midpoint: [0, 2, 0],
      support: 12,
    })], [])).toHaveLength(0);
    expect(certifyOperationalStructuralEdges([edge({
      start: [-0.75, 2, 0],
      end: [0.75, 2, 0],
      midpoint: [0, 2, 0],
      support: 48,
    })], [])).toHaveLength(1);
  });

  it('can certify several short mature fragments only after they form one supported connected line', () => {
    const certified = certifyOperationalStructuralEdges([
      fragment('edge:e1', 0.00, 0.12, 2, 8),
      fragment('edge:e2', 0.10, 0.23, 2, 8),
      fragment('edge:e3', 0.21, 0.35, 2, 8),
    ], []);
    expect(certified).toHaveLength(1);
    expect(Math.abs(certified[0].end[0] - certified[0].start[0])).toBeGreaterThan(0.30);
  });

  it('does not manufacture physical observation maturity from positive visual matches', () => {
    const frames = [
      visuallyInconsistentFrame(1, 0),
      visuallyInconsistentFrame(2, 0.10),
      visuallyInconsistentFrame(3, 0.20),
    ].map((frame) => ({
      ...frame,
      lines: frame.lines.map((line) => ({
        ...line,
        start: [line.start[0], 0.5] as const,
        end: [line.end[0], 0.5] as const,
      })),
    }));
    const certified = certifyOperationalStructuralEdges([edge({ observations: 2, confidence: 0.75 })], frames);
    expect(certified).toHaveLength(1);
    expect(certified[0].observations).toBe(2);
  });
});
