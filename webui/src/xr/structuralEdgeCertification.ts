import { certifyEdgeByVisualReprojection } from './reprojectionCertificate';
import type { StructuralEdgeCandidate } from './structuralGuide';
import { applyStructuralVisualCorroboration } from './structuralVisualCorroboration';
import type { VisualEvidenceFrame } from './visualLineEvidence';

export const MIN_CERTIFIED_EDGE_CONFIDENCE = 0.66;
export const MIN_CERTIFIED_EDGE_OBSERVATIONS = 2;
export const MIN_CERTIFIED_EDGE_LENGTH_METERS = 0.065;
const MIN_VISUAL_STRUCTURE_STRENGTH = 0.30;
const MIN_VISUAL_FRAMES = 3;

function distance(
  start: readonly [number, number, number],
  end: readonly [number, number, number],
): number {
  return Math.hypot(end[0] - start[0], end[1] - start[1], end[2] - start[2]);
}

function hasUsableLineCoverage(frames: readonly VisualEvidenceFrame[]): boolean {
  return frames.filter((frame) => frame.lines.some((line) => line.strength >= MIN_VISUAL_STRUCTURE_STRENGTH)).length >= MIN_VISUAL_FRAMES;
}

/**
 * Final edge gate before the shared manual/overlay registry.
 * Metric confidence, finite support and physical observation count are hard
 * requirements. RGB can corroborate but a Hough miss cannot erase a mature
 * ARCore edge; image-space line detection is not a reliable negative sensor.
 */
export function certifyOperationalStructuralEdges(
  edges: readonly StructuralEdgeCandidate[],
  visualFrames: readonly VisualEvidenceFrame[],
): readonly StructuralEdgeCandidate[] {
  const checkVisual = hasUsableLineCoverage(visualFrames);
  const output: StructuralEdgeCandidate[] = [];
  for (const edge of edges) {
    if (edge.confidence < MIN_CERTIFIED_EDGE_CONFIDENCE) continue;
    if (edge.observations < MIN_CERTIFIED_EDGE_OBSERVATIONS) continue;
    const length = distance(edge.start, edge.end);
    if (!Number.isFinite(length) || length < MIN_CERTIFIED_EDGE_LENGTH_METERS) continue;
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
