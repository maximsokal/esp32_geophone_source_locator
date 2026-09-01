import type { ProjectPoint } from './manualControlPoints';
import { projectProjectPointToNormalized } from './projectionMath';
import type { VisualEvidenceFrame, VisualLineSegment } from './visualLineEvidence';

export interface ReprojectionCertificate {
  accepted: boolean;
  observations: number;
  meanErrorNormalized: number;
  confidence: number;
  matchedKeyframes: readonly number[];
}

const DEFAULT_CORNER_TOLERANCE = 0.035;
const DEFAULT_ENDPOINT_TOLERANCE = 0.030;
const DEFAULT_EDGE_TOLERANCE = 0.028;
const MIN_BASELINE_METERS = 0.075;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }

function cameraProjectPosition(frame: VisualEvidenceFrame): ProjectPoint {
  return [frame.worldFromView[12], -frame.worldFromView[14], frame.worldFromView[13]];
}

function distance3(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function pointSegmentDistance(u: number, v: number, line: VisualLineSegment): number {
  const dx = line.end[0] - line.start[0];
  const dy = line.end[1] - line.start[1];
  const squared = dx * dx + dy * dy;
  if (squared < 1e-12) return Math.hypot(u - line.start[0], v - line.start[1]);
  const t = Math.max(0, Math.min(1, ((u - line.start[0]) * dx + (v - line.start[1]) * dy) / squared));
  return Math.hypot(u - (line.start[0] + dx * t), v - (line.start[1] + dy * t));
}

function lineEndpointDistance(u: number, v: number, line: VisualLineSegment): number {
  return Math.min(
    Math.hypot(u - line.start[0], v - line.start[1]),
    Math.hypot(u - line.end[0], v - line.end[1]),
  );
}

function independentFrame(acceptedFrames: readonly VisualEvidenceFrame[], candidate: VisualEvidenceFrame): boolean {
  const camera = cameraProjectPosition(candidate);
  return acceptedFrames.every((frame) => distance3(camera, cameraProjectPosition(frame)) >= MIN_BASELINE_METERS);
}

function finishCertificate(
  acceptedFrames: readonly VisualEvidenceFrame[],
  errors: readonly number[],
  strengths: readonly number[],
  minimumObservations: number,
  toleranceNormalized: number,
): ReprojectionCertificate {
  const observations = acceptedFrames.length;
  const meanError = observations > 0
    ? errors.reduce((a, b) => a + b, 0) / observations
    : Number.POSITIVE_INFINITY;
  const meanStrength = observations > 0
    ? strengths.reduce((a, b) => a + b, 0) / observations
    : 0;
  const countScore = clamp01(observations / Math.max(minimumObservations, 1));
  const errorScore = Number.isFinite(meanError) ? clamp01(1 - meanError / toleranceNormalized) : 0;
  return {
    accepted: observations >= minimumObservations,
    observations,
    meanErrorNormalized: meanError,
    confidence: clamp01(countScore * 0.46 + errorScore * 0.34 + meanStrength * 0.20),
    matchedKeyframes: acceptedFrames.map((frame) => frame.keyframeId),
  };
}

export function certifyCornerByVisualReprojection(
  position: ProjectPoint,
  frames: readonly VisualEvidenceFrame[],
  minimumObservations = 3,
  toleranceNormalized = DEFAULT_CORNER_TOLERANCE,
): ReprojectionCertificate {
  const acceptedFrames: VisualEvidenceFrame[] = [];
  const errors: number[] = [];
  const strengths: number[] = [];
  for (const frame of frames) {
    const projection = projectProjectPointToNormalized(position, frame.projectionMatrix, frame.worldFromView);
    if (projection === null || !projection.visible || frame.junctions.length === 0) continue;
    let bestError = Number.POSITIVE_INFINITY;
    let bestStrength = 0;
    for (const junction of frame.junctions) {
      const error = Math.hypot(junction.position[0] - projection.u, junction.position[1] - projection.v);
      if (error < bestError) { bestError = error; bestStrength = junction.strength; }
    }
    if (bestError > toleranceNormalized || !independentFrame(acceptedFrames, frame)) continue;
    acceptedFrames.push(frame);
    errors.push(bestError);
    strengths.push(bestStrength);
  }
  return finishCertificate(acceptedFrames, errors, strengths, minimumObservations, toleranceNormalized);
}

/**
 * Finite two-plane endpoints do not always form a 2D junction (a visible edge
 * can simply stop at an occlusion/boundary). Accept endpoint evidence only when
 * the projected 3D endpoint repeatedly lands near a detected line endpoint in
 * independent RGB keyframes.
 */
export function certifyEndpointByVisualReprojection(
  position: ProjectPoint,
  frames: readonly VisualEvidenceFrame[],
  minimumObservations = 3,
  toleranceNormalized = DEFAULT_ENDPOINT_TOLERANCE,
): ReprojectionCertificate {
  const acceptedFrames: VisualEvidenceFrame[] = [];
  const errors: number[] = [];
  const strengths: number[] = [];
  for (const frame of frames) {
    const projection = projectProjectPointToNormalized(position, frame.projectionMatrix, frame.worldFromView);
    if (projection === null || !projection.visible || frame.lines.length === 0) continue;
    let bestError = Number.POSITIVE_INFINITY;
    let bestStrength = 0;
    for (const line of frame.lines) {
      const error = lineEndpointDistance(projection.u, projection.v, line);
      if (error < bestError) { bestError = error; bestStrength = line.strength; }
    }
    if (bestError > toleranceNormalized || !independentFrame(acceptedFrames, frame)) continue;
    acceptedFrames.push(frame);
    errors.push(bestError);
    strengths.push(bestStrength);
  }
  return finishCertificate(acceptedFrames, errors, strengths, minimumObservations, toleranceNormalized);
}

export function certifyEdgeByVisualReprojection(
  start: ProjectPoint,
  end: ProjectPoint,
  frames: readonly VisualEvidenceFrame[],
  minimumObservations = 3,
  toleranceNormalized = DEFAULT_EDGE_TOLERANCE,
): ReprojectionCertificate {
  const acceptedFrames: VisualEvidenceFrame[] = [];
  const errors: number[] = [];
  const strengths: number[] = [];
  for (const frame of frames) {
    const a = projectProjectPointToNormalized(start, frame.projectionMatrix, frame.worldFromView);
    const b = projectProjectPointToNormalized(end, frame.projectionMatrix, frame.worldFromView);
    if (a === null || b === null || !a.visible || !b.visible || frame.lines.length === 0) continue;
    const projectedDx = b.u - a.u;
    const projectedDy = b.v - a.v;
    const projectedLength = Math.hypot(projectedDx, projectedDy);
    if (projectedLength < 0.025) continue;
    const dirX = projectedDx / projectedLength;
    const dirY = projectedDy / projectedLength;
    let bestError = Number.POSITIVE_INFINITY;
    let bestStrength = 0;
    for (const line of frame.lines) {
      const directionDot = Math.abs(dirX * line.direction[0] + dirY * line.direction[1]);
      if (directionDot < Math.cos((12 * Math.PI) / 180)) continue;
      const midpointU = (a.u + b.u) * 0.5;
      const midpointV = (a.v + b.v) * 0.5;
      const midpointError = pointSegmentDistance(midpointU, midpointV, line);
      const endpointError = Math.min(
        (pointSegmentDistance(a.u, a.v, line) + pointSegmentDistance(b.u, b.v, line)) * 0.5,
        midpointError * 1.5,
      );
      const error = midpointError * 0.65 + endpointError * 0.35;
      if (error < bestError) { bestError = error; bestStrength = line.strength * directionDot; }
    }
    if (bestError > toleranceNormalized || !independentFrame(acceptedFrames, frame)) continue;
    acceptedFrames.push(frame);
    errors.push(bestError);
    strengths.push(bestStrength);
  }
  return finishCertificate(acceptedFrames, errors, strengths, minimumObservations, toleranceNormalized);
}
