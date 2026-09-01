import { describe, expect, it } from 'vitest';

import { StructuralLifecycleTelemetry } from '../../src/xr/structuralLifecycleTelemetry';
import type { StructuralCornerCandidate, StructuralEdgeCandidate } from '../../src/xr/structuralGuide';

function edge(id: string): StructuralEdgeCandidate {
  return {
    id,
    kind: 'edge',
    start: [0, 0, 0],
    end: [0.2, 0, 0],
    midpoint: [0.1, 0, 0],
    direction: [1, 0, 0],
    confidence: 0.7,
    geometryConfidence: 0.65,
    support: 12,
    observations: 3,
  };
}

function corner(id: string): StructuralCornerCandidate {
  return {
    id,
    kind: 'corner',
    position: [0, 0, 0],
    confidence: 0.75,
    geometryConfidence: 0.72,
    support: 18,
    observations: 3,
  };
}

describe('structural hypothesis lifecycle telemetry', () => {
  it('counts creation, persistence and expiry across worker analyses', () => {
    const telemetry = new StructuralLifecycleTelemetry();

    expect(telemetry.update([edge('e1'), edge('e2')], [corner('c1')])).toMatchObject({
      edgeHypothesesActive: 2,
      edgeHypothesesCreated: 2,
      edgeHypothesesRetainedTransitions: 0,
      edgeHypothesesExpired: 0,
      cornerHypothesesCreated: 1,
    });

    expect(telemetry.update([edge('e2'), edge('e3')], [corner('c1')])).toMatchObject({
      edgeHypothesesActive: 2,
      edgeHypothesesCreated: 3,
      edgeHypothesesRetainedTransitions: 1,
      edgeHypothesesExpired: 1,
      cornerHypothesesCreated: 1,
      cornerHypothesesRetainedTransitions: 1,
      cornerHypothesesExpired: 0,
    });
  });

  it('resets cumulative lifecycle counters for a new XR epoch', () => {
    const telemetry = new StructuralLifecycleTelemetry();
    telemetry.update([edge('e1')], [corner('c1')]);
    telemetry.update([], []);
    telemetry.reset();

    expect(telemetry.update([edge('e2')], [])).toEqual({
      edgeHypothesesActive: 1,
      edgeHypothesesCreated: 1,
      edgeHypothesesRetainedTransitions: 0,
      edgeHypothesesExpired: 0,
      cornerHypothesesActive: 0,
      cornerHypothesesCreated: 0,
      cornerHypothesesRetainedTransitions: 0,
      cornerHypothesesExpired: 0,
    });
  });
});
