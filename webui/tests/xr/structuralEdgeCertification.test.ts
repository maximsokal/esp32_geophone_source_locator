import { describe, expect, it } from 'vitest';

import { certifyOperationalStructuralEdges } from '../../src/xr/structuralEdgeCertification';
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
