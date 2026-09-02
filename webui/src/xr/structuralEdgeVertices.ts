import type {
  ProjectPoint,
  StructuralCornerCandidate,
  StructuralEdgeCandidate,
} from './structuralGuide';
import { isScaleSupportedStructuralEdge } from './structuralEdgeScalePolicy';

const MIN_EDGE_CONFIDENCE = 0.68;
const MIN_EDGE_GEOMETRY_CONFIDENCE = 0.70;
const MIN_EDGE_OBSERVATIONS = 3;
const MIN_ENDPOINT_JOIN_METERS = 0.025;
const MAX_ENDPOINT_JOIN_METERS = 0.060;
const ENDPOINT_JOIN_SHORT_EDGE_FRACTION = 0.22;
const MAX_PARALLEL_DIRECTION_DOT = Math.cos((35 * Math.PI) / 180);
const DEDUPE_METERS = 0.075;
const MAX_VERTICES = 8;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}
function distance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
function edgeLength(edge: StructuralEdgeCandidate): number {
  return distance(edge.start, edge.end);
}
function endpointJoinGateMeters(first: StructuralEdgeCandidate, second: StructuralEdgeCandidate): number {
  const shorter = Math.min(edgeLength(first), edgeLength(second));
  return clamp(
    shorter * ENDPOINT_JOIN_SHORT_EDGE_FRACTION,
    MIN_ENDPOINT_JOIN_METERS,
    MAX_ENDPOINT_JOIN_METERS,
  );
}
function normalize(vector: readonly [number, number, number]): readonly [number, number, number] | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return magnitude > 1e-8 ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude] : null;
}
function direction(edge: StructuralEdgeCandidate): readonly [number, number, number] | null {
  return normalize([
    edge.end[0] - edge.start[0],
    edge.end[1] - edge.start[1],
    edge.end[2] - edge.start[2],
  ]);
}
function midpoint(a: ProjectPoint, b: ProjectPoint): ProjectPoint {
  return [(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5];
}
function vertexId(position: ProjectPoint): string {
  const q = 0.06;
  return `edge-vertex:${Math.round(position[0] / q)}:${Math.round(position[1] / q)}:${Math.round(position[2] / q)}`;
}

interface EndpointJoin {
  a: ProjectPoint;
  b: ProjectPoint;
  gap: number;
}

function nearestEndpointJoin(first: StructuralEdgeCandidate, second: StructuralEdgeCandidate): EndpointJoin {
  const pairs: readonly [ProjectPoint, ProjectPoint][] = [
    [first.start, second.start],
    [first.start, second.end],
    [first.end, second.start],
    [first.end, second.end],
  ];
  let best: EndpointJoin = { a: pairs[0][0], b: pairs[0][1], gap: distance(pairs[0][0], pairs[0][1]) };
  for (let index = 1; index < pairs.length; index += 1) {
    const gap = distance(pairs[index][0], pairs[index][1]);
    if (gap < best.gap) best = { a: pairs[index][0], b: pairs[index][1], gap };
  }
  return best;
}

function mature(edge: StructuralEdgeCandidate): boolean {
  return edge.confidence >= MIN_EDGE_CONFIDENCE &&
    edge.geometryConfidence >= MIN_EDGE_GEOMETRY_CONFIDENCE &&
    edge.observations >= MIN_EDGE_OBSERVATIONS &&
    isScaleSupportedStructuralEdge(edge);
}

/**
 * Infer a finite structural vertex from two mature metric 3D edge segments.
 * This covers real furniture/building corners where the depth sensor has a hole
 * exactly at the vertex and only two terminating edges remain measurable.
 * Infinite-line intersections are deliberately not used: both measured segment
 * endpoints must physically converge near the proposed vertex. The allowed hole
 * is proportional to the SHORTER measured edge, so two barely-long-enough 9-cm
 * fragments cannot bridge the old universal 10-cm gap. Long, densely supported
 * edges may tolerate a larger sensor hole, but never more than 6 cm; therefore
 * the inferred midpoint stays within 3 cm of measured finite support.
 *
 * The same length-dependent support-density policy used by operational line
 * certification is enforced again here so downstream callers cannot bypass
 * physical maturity.
 */
export function inferStructuralVerticesFromEdges(
  edges: readonly StructuralEdgeCandidate[],
): readonly StructuralCornerCandidate[] {
  const vertices: StructuralCornerCandidate[] = [];

  for (let firstIndex = 0; firstIndex < edges.length; firstIndex += 1) {
    const first = edges[firstIndex];
    if (!mature(first)) continue;
    const firstDirection = direction(first);
    if (firstDirection === null) continue;

    for (let secondIndex = firstIndex + 1; secondIndex < edges.length; secondIndex += 1) {
      const second = edges[secondIndex];
      if (!mature(second)) continue;
      const secondDirection = direction(second);
      if (secondDirection === null) continue;

      const directionDot = Math.abs(
        firstDirection[0] * secondDirection[0] +
        firstDirection[1] * secondDirection[1] +
        firstDirection[2] * secondDirection[2]
      );
      if (directionDot > MAX_PARALLEL_DIRECTION_DOT) continue;

      const join = nearestEndpointJoin(first, second);
      const joinGateMeters = endpointJoinGateMeters(first, second);
      if (!Number.isFinite(join.gap) || join.gap > joinGateMeters) continue;
      const position = midpoint(join.a, join.b);
      const joinScore = clamp01(1 - join.gap / joinGateMeters);
      const angleScore = clamp01(1 - directionDot / MAX_PARALLEL_DIRECTION_DOT);
      const physicalConfidence = Math.min(first.confidence, second.confidence);
      const geometryConfidence = Math.min(first.geometryConfidence, second.geometryConfidence);
      const confidence = Math.min(0.95, clamp01(
        physicalConfidence * 0.72 + geometryConfidence * 0.20 + joinScore * 0.06 + angleScore * 0.02,
      ));
      if (confidence < 0.70) continue;

      const candidate: StructuralCornerCandidate = {
        id: vertexId(position),
        kind: 'corner',
        position,
        confidence,
        geometryConfidence,
        support: first.support + second.support,
        observations: Math.min(first.observations, second.observations),
      };
      const existing = vertices.find((item) => distance(item.position, position) <= DEDUPE_METERS);
      if (existing === undefined) vertices.push(candidate);
      else if (candidate.confidence > existing.confidence) vertices[vertices.indexOf(existing)] = candidate;
    }
  }

  return vertices
    .sort((left, right) => right.confidence - left.confidence)
    .slice(0, MAX_VERTICES);
}
