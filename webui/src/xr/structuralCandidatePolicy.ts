import type { StructuralCornerCandidate, StructuralEdgeCandidate } from './structuralGuide';

export interface StructuralCandidateDiagnostics {
  rawTrackedEdges: number;
  operationalEdges: number;
  rawEdgeMinConfidence: number | null;
  rawEdgeMaxConfidence: number | null;
  operationalEdgeMinConfidence: number | null;
  operationalEdgeMaxConfidence: number | null;
  edgeMaxObservations: number;
  edgeMaxGeometryConfidence: number | null;
  threePlaneCorners: number;
  endpointCorners: number;
}

const GEOMETRY_WEIGHT = 0.62;
const MATURITY_WEIGHT = 0.38;
const MATURITY_FULL_OBSERVATIONS = 5;

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

/**
 * StructuralGuideTracker already combines geometry with temporal evidence, but
 * its historical 68/32 split can leave a repeatedly observed, valid two-plane
 * edge permanently just below the UI's 0.66 operational gate. For an edge that
 * survives five independent views, persistence itself is strong evidence.
 *
 * This is not a threshold bypass: geometry still contributes 62%, observations
 * must be independent (enforced by the tracker), RGB can still candidate-veto a
 * geometrically inconsistent edge, and the caller keeps the stricter original
 * confidence whenever it is already larger.
 */
export function maturityAwareEdgeConfidence(edge: StructuralEdgeCandidate): number {
  const maturity = clamp01((edge.observations - 1) / Math.max(1, MATURITY_FULL_OBSERVATIONS - 1));
  const persistentGeometry = clamp01(
    clamp01(edge.geometryConfidence) * GEOMETRY_WEIGHT + maturity * MATURITY_WEIGHT,
  );
  return Math.max(clamp01(edge.confidence), persistentGeometry);
}

export function applyMaturityAwareEdgeConfidence(edge: StructuralEdgeCandidate): StructuralEdgeCandidate {
  const confidence = maturityAwareEdgeConfidence(edge);
  return confidence === edge.confidence ? edge : { ...edge, confidence };
}

function extrema(values: readonly number[]): readonly [number | null, number | null] {
  if (values.length === 0) return [null, null];
  return [Math.min(...values), Math.max(...values)];
}

function rounded(value: number | null): number | null {
  return value === null ? null : Number(value.toFixed(3));
}

export function summarizeStructuralCandidates(
  rawEdges: readonly StructuralEdgeCandidate[],
  operationalEdges: readonly StructuralEdgeCandidate[],
  corners: readonly StructuralCornerCandidate[],
): StructuralCandidateDiagnostics {
  const [rawMin, rawMax] = extrema(rawEdges.map((edge) => edge.confidence));
  const [operationalMin, operationalMax] = extrema(operationalEdges.map((edge) => edge.confidence));
  const geometryMax = rawEdges.length === 0
    ? null
    : Math.max(...rawEdges.map((edge) => edge.geometryConfidence));
  return {
    rawTrackedEdges: rawEdges.length,
    operationalEdges: operationalEdges.length,
    rawEdgeMinConfidence: rounded(rawMin),
    rawEdgeMaxConfidence: rounded(rawMax),
    operationalEdgeMinConfidence: rounded(operationalMin),
    operationalEdgeMaxConfidence: rounded(operationalMax),
    edgeMaxObservations: rawEdges.reduce((maximum, edge) => Math.max(maximum, edge.observations), 0),
    edgeMaxGeometryConfidence: rounded(geometryMax),
    threePlaneCorners: corners.filter((corner) => !corner.id.startsWith('endpoint:')).length,
    endpointCorners: corners.filter((corner) => corner.id.startsWith('endpoint:')).length,
  };
}
