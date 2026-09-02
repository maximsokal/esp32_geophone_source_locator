import { certifyEdgeByVisualReprojection } from './reprojectionCertificate';
import type { ProjectPoint, ProjectVector, StructuralEdgeCandidate } from './structuralGuide';
import { applyStructuralVisualCorroboration } from './structuralVisualCorroboration';
import type { VisualEvidenceFrame } from './visualLineEvidence';

export const MIN_CERTIFIED_EDGE_CONFIDENCE = 0.66;
export const MIN_CERTIFIED_EDGE_OBSERVATIONS = 2;
// A structural line shorter than roughly two nominal 4-cm source voxels is not
// a useful metric constraint. Local patch fragments may be shorter and are
// merged before this final gate.
export const MIN_CERTIFIED_EDGE_LENGTH_METERS = 0.09;
const MIN_LOCAL_EDGE_FRAGMENT_METERS = 0.035;
const MIN_VISUAL_STRUCTURE_STRENGTH = 0.30;
const MIN_VISUAL_FRAMES = 3;
const MERGE_DIRECTION_DOT = Math.cos((15 * Math.PI) / 180);
const MERGE_MAX_LATERAL_METERS = 0.055;
const MERGE_MAX_AXIAL_GAP_METERS = 0.085;
const MIN_SPAN_COVERAGE_RATIO = 0.70;
const MIN_EDGE_SUPPORT = 6;
const EDGE_SUPPORT_INTERVAL_METERS = 0.075;
const MIN_SUPPORT_DENSITY_PER_METER = 22;

interface Interval {
  min: number;
  max: number;
}

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }

function distance(start: ProjectPoint, end: ProjectPoint): number {
  return Math.hypot(end[0] - start[0], end[1] - start[1], end[2] - start[2]);
}

function dot(a: ProjectVector, b: ProjectVector): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function normalize(vector: ProjectVector): ProjectVector | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return Number.isFinite(magnitude) && magnitude > 1e-9
    ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude]
    : null;
}

function midpoint(start: ProjectPoint, end: ProjectPoint): ProjectPoint {
  return [
    (start[0] + end[0]) * 0.5,
    (start[1] + end[1]) * 0.5,
    (start[2] + end[2]) * 0.5,
  ];
}

function addScaled(origin: ProjectPoint, direction: ProjectVector, scale: number): ProjectPoint {
  return [
    origin[0] + direction[0] * scale,
    origin[1] + direction[1] * scale,
    origin[2] + direction[2] * scale,
  ];
}

function subtract(a: ProjectPoint, b: ProjectPoint): ProjectVector {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function projectedInterval(edge: StructuralEdgeCandidate, origin: ProjectPoint, direction: ProjectVector): Interval {
  const a = dot(subtract(edge.start, origin), direction);
  const b = dot(subtract(edge.end, origin), direction);
  return { min: Math.min(a, b), max: Math.max(a, b) };
}

function intervalGap(a: Interval, b: Interval): number {
  if (a.max < b.min) return b.min - a.max;
  if (b.max < a.min) return a.min - b.max;
  return 0;
}

function lateralDistance(edge: StructuralEdgeCandidate, origin: ProjectPoint, direction: ProjectVector): number {
  const delta = subtract(edge.midpoint, origin);
  const axial = dot(delta, direction);
  const lateral: ProjectVector = [
    delta[0] - direction[0] * axial,
    delta[1] - direction[1] * axial,
    delta[2] - direction[2] * axial,
  ];
  return Math.hypot(...lateral);
}

function edgesConnect(a: StructuralEdgeCandidate, b: StructuralEdgeCandidate): boolean {
  if (Math.abs(dot(a.direction, b.direction)) < MERGE_DIRECTION_DOT) return false;
  const direction = normalize(a.direction);
  if (direction === null) return false;
  // Test both segment centrelines. A single midpoint distance is insufficient
  // for long segments that partially overlap near one end.
  if (
    lateralDistance(b, a.midpoint, direction) > MERGE_MAX_LATERAL_METERS ||
    lateralDistance(a, b.midpoint, direction) > MERGE_MAX_LATERAL_METERS
  ) return false;
  return intervalGap(
    projectedInterval(a, a.midpoint, direction),
    projectedInterval(b, a.midpoint, direction),
  ) <= MERGE_MAX_AXIAL_GAP_METERS;
}

function unionLength(intervals: readonly Interval[]): number {
  if (intervals.length === 0) return 0;
  const sorted = [...intervals].sort((a, b) => a.min - b.min || a.max - b.max);
  let total = 0;
  let start = sorted[0].min;
  let end = sorted[0].max;
  for (let index = 1; index < sorted.length; index += 1) {
    const next = sorted[index];
    if (next.min <= end) {
      end = Math.max(end, next.max);
      continue;
    }
    total += Math.max(0, end - start);
    start = next.min;
    end = next.max;
  }
  return total + Math.max(0, end - start);
}

function mergeGroup(group: readonly StructuralEdgeCandidate[]): StructuralEdgeCandidate | null {
  if (group.length === 0) return null;
  const anchor = [...group].sort((a, b) => a.id.localeCompare(b.id))[0];
  let dx = 0; let dy = 0; let dz = 0; let weightSum = 0;
  let ox = 0; let oy = 0; let oz = 0;
  let confidenceWeighted = 0;
  let geometryWeighted = 0;
  let supportSum = 0;
  let strongestSupport = 0;
  let observations = Number.POSITIVE_INFINITY;

  const reference = normalize(anchor.direction);
  if (reference === null) return null;
  for (const edge of group) {
    const edgeDirection = normalize(edge.direction);
    if (edgeDirection === null) continue;
    const sign = dot(reference, edgeDirection) < 0 ? -1 : 1;
    const length = Math.max(MIN_LOCAL_EDGE_FRAGMENT_METERS, distance(edge.start, edge.end));
    const weight = length * Math.max(0.25, edge.confidence);
    dx += edgeDirection[0] * sign * weight;
    dy += edgeDirection[1] * sign * weight;
    dz += edgeDirection[2] * sign * weight;
    ox += edge.midpoint[0] * weight;
    oy += edge.midpoint[1] * weight;
    oz += edge.midpoint[2] * weight;
    confidenceWeighted += edge.confidence * weight;
    geometryWeighted += edge.geometryConfidence * weight;
    weightSum += weight;
    supportSum += Math.max(0, edge.support);
    strongestSupport = Math.max(strongestSupport, edge.support);
    observations = Math.min(observations, edge.observations);
  }
  if (weightSum <= 1e-9) return null;
  const direction = normalize([dx, dy, dz]);
  if (direction === null) return null;
  const origin: ProjectPoint = [ox / weightSum, oy / weightSum, oz / weightSum];
  const intervals = group.map((edge) => projectedInterval(edge, origin, direction));
  const low = Math.min(...intervals.map((interval) => interval.min));
  const high = Math.max(...intervals.map((interval) => interval.max));
  const span = high - low;
  if (!Number.isFinite(span) || span <= 0) return null;
  const coverageRatio = clamp01(unionLength(intervals) / span);
  const effectiveSupport = Math.max(
    strongestSupport,
    Math.round(supportSum * (0.55 + coverageRatio * 0.25)),
  );
  const start = addScaled(origin, direction, low);
  const end = addScaled(origin, direction, high);
  return {
    id: anchor.id,
    kind: 'edge',
    start,
    end,
    midpoint: midpoint(start, end),
    direction,
    confidence: confidenceWeighted / weightSum,
    geometryConfidence: geometryWeighted / weightSum,
    support: effectiveSupport,
    observations: Number.isFinite(observations) ? observations : 0,
  };
}

/**
 * Join only already-mature physical edge fragments. This recovers the measured
 * scale of a long structural edge that crosses several 10-cm plane-patch cells
 * without allowing an untracked fragment to borrow maturity from its neighbour.
 */
export function mergeConnectedStructuralEdges(
  edges: readonly StructuralEdgeCandidate[],
): readonly StructuralEdgeCandidate[] {
  const candidates = [...edges]
    .filter((edge) =>
      edge.confidence >= MIN_CERTIFIED_EDGE_CONFIDENCE &&
      edge.observations >= MIN_CERTIFIED_EDGE_OBSERVATIONS &&
      distance(edge.start, edge.end) >= MIN_LOCAL_EDGE_FRAGMENT_METERS
    )
    .sort((a, b) => a.id.localeCompare(b.id));
  const visited = new Uint8Array(candidates.length);
  const merged: StructuralEdgeCandidate[] = [];
  for (let seed = 0; seed < candidates.length; seed += 1) {
    if (visited[seed] !== 0) continue;
    visited[seed] = 1;
    const queue = [seed];
    const group: StructuralEdgeCandidate[] = [];
    for (let read = 0; read < queue.length; read += 1) {
      const currentIndex = queue[read];
      const current = candidates[currentIndex];
      group.push(current);
      for (let candidateIndex = 0; candidateIndex < candidates.length; candidateIndex += 1) {
        if (visited[candidateIndex] !== 0) continue;
        if (!edgesConnect(current, candidates[candidateIndex])) continue;
        visited[candidateIndex] = 1;
        queue.push(candidateIndex);
      }
    }
    const combined = mergeGroup(group);
    if (combined !== null) merged.push(combined);
  }
  return merged.sort((a, b) => b.confidence - a.confidence || a.id.localeCompare(b.id));
}

function hasUsableLineCoverage(frames: readonly VisualEvidenceFrame[]): boolean {
  return frames.filter((frame) => frame.lines.some((line) => line.strength >= MIN_VISUAL_STRUCTURE_STRENGTH)).length >= MIN_VISUAL_FRAMES;
}

function scaleSupportAcceptable(edge: StructuralEdgeCandidate): boolean {
  const length = distance(edge.start, edge.end);
  if (!Number.isFinite(length) || length < MIN_CERTIFIED_EDGE_LENGTH_METERS) return false;
  // Support must grow with measured line length. The absolute floor protects
  // short edges; the interval term prevents a metre-long line from being
  // certified by the same handful of points that would justify a 10-cm edge.
  const requiredSupport = Math.max(
    MIN_EDGE_SUPPORT,
    Math.ceil(length / EDGE_SUPPORT_INTERVAL_METERS) * 2,
  );
  const supportDensity = edge.support / Math.max(length, MIN_CERTIFIED_EDGE_LENGTH_METERS);
  return edge.support >= requiredSupport && supportDensity >= MIN_SUPPORT_DENSITY_PER_METER;
}

/**
 * Final edge gate before the shared manual/overlay registry.
 *
 * The gate is deliberately multi-parameter: every local fragment must already
 * have metric confidence and independent physical observations; compatible
 * fragments are then joined into the measured connected line; the final line
 * must have useful metric length AND support that scales with that length. RGB
 * may corroborate the surviving metric edge but never creates observations or
 * compensates for insufficient physical support.
 */
export function certifyOperationalStructuralEdges(
  edges: readonly StructuralEdgeCandidate[],
  visualFrames: readonly VisualEvidenceFrame[],
): readonly StructuralEdgeCandidate[] {
  const checkVisual = hasUsableLineCoverage(visualFrames);
  const output: StructuralEdgeCandidate[] = [];
  for (const edge of mergeConnectedStructuralEdges(edges)) {
    if (!scaleSupportAcceptable(edge)) continue;
    if (!checkVisual) {
      output.push(edge);
      continue;
    }
    const visual = certifyEdgeByVisualReprojection(edge.start, edge.end, visualFrames);
    const corroborated = applyStructuralVisualCorroboration(edge.confidence, edge.observations, visual);
    output.push({
      ...edge,
      confidence: corroborated.confidence,
      // Keep metric observations physical-only; RGB count is deliberately not
      // copied into the StructuralEdgeCandidate maturity channel.
      observations: edge.observations,
    });
  }
  return output.sort((left, right) => right.confidence - left.confidence).slice(0, 10);
}
