import { describe, expect, it } from 'vitest';

import {
  applyMaturityAwareEdgeConfidence,
  maturityAwareEdgeConfidence,
  summarizeStructuralCandidates,
} from '../../src/xr/structuralCandidatePolicy';
import type { StructuralCornerCandidate, StructuralEdgeCandidate } from '../../src/xr/structuralGuide';

function edge(
  observations: number,
  geometryConfidence: number,
  confidence: number,
): StructuralEdgeCandidate {
  return {
    id: `edge:${observations}:${geometryConfidence}`,
    kind: 'edge',
    start: [0, 0, 0],
    end: [0.2, 0, 0],
    midpoint: [0.1, 0, 0],
    direction: [1, 0, 0],
    confidence,
    geometryConfidence,
    support: 12,
    observations,
  };
}

function corner(id: string): StructuralCornerCandidate {
  return {
    id,
    kind: 'corner',
    position: [0, 0, 0],
    confidence: 0.8,
    geometryConfidence: 0.8,
    support: 18,
    observations: 3,
  };
}

describe('maturity-aware structural edge policy', () => {
  it('does not manufacture a high-confidence edge from only two weak observations', () => {
    const candidate = edge(2, 0.48, 0.58);
    expect(maturityAwareEdgeConfidence(candidate)).toBeLessThan(0.66);
  });

  it('lets a minimum-valid two-plane edge become operational after five independent observations', () => {
    const candidate = edge(5, 0.48, 0.646);
    const promoted = applyMaturityAwareEdgeConfidence(candidate);
    expect(promoted.confidence).toBeGreaterThanOrEqual(0.66);
    expect(promoted.geometryConfidence).toBe(candidate.geometryConfidence);
    expect(promoted.observations).toBe(candidate.observations);
  });

  it('never lowers an already stronger tracker confidence', () => {
    const candidate = edge(3, 0.92, 0.91);
    expect(maturityAwareEdgeConfidence(candidate)).toBe(0.91);
    expect(applyMaturityAwareEdgeConfidence(candidate)).toBe(candidate);
  });

  it('is monotonic with independent-view maturity for equal geometry', () => {
    const values = [2, 3, 4, 5].map((observations) =>
      maturityAwareEdgeConfidence(edge(observations, 0.52, 0.58)),
    );
    expect(values[1]).toBeGreaterThanOrEqual(values[0]);
    expect(values[2]).toBeGreaterThanOrEqual(values[1]);
    expect(values[3]).toBeGreaterThanOrEqual(values[2]);
  });

  it('summarizes raw-to-operational gates without confusing endpoint and three-plane corners', () => {
    const raw = [edge(2, 0.55, 0.59), edge(5, 0.48, 0.646)];
    const operational = raw.map(applyMaturityAwareEdgeConfidence);
    const diagnostics = summarizeStructuralCandidates(
      raw,
      operational,
      [corner('corner:c1'), corner('endpoint:e1')],
    );
    expect(diagnostics.rawTrackedEdges).toBe(2);
    expect(diagnostics.operationalEdges).toBe(2);
    expect(diagnostics.edgeMaxObservations).toBe(5);
    expect(diagnostics.operationalEdgeMaxConfidence).toBeGreaterThanOrEqual(0.66);
    expect(diagnostics.threePlaneCorners).toBe(1);
    expect(diagnostics.endpointCorners).toBe(1);
  });
});
