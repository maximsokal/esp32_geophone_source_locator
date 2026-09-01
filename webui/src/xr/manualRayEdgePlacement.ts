import type { ProjectPoint } from './manualControlPoints';
import type { CertifiedGuideEdge } from './certifiedCornerRegistry';

export type ProjectVector = readonly [number, number, number];

export interface ProjectRay {
  origin: ProjectPoint;
  direction: ProjectVector;
}

export interface RayEdgePlacementResult {
  accepted: boolean;
  position: ProjectPoint | null;
  edgeId: string | null;
  confidence: number;
  skewDistanceMeters: number | null;
  condition: number | null;
  segmentParameter: number | null;
  message: string;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function dot(a: ProjectVector, b: ProjectVector): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function normalize(vector: ProjectVector): ProjectVector | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return magnitude > 1e-8
    ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude]
    : null;
}

function subtract(a: ProjectPoint, b: ProjectPoint): ProjectVector {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function addScaled(point: ProjectPoint, direction: ProjectVector, amount: number): ProjectPoint {
  return [point[0] + direction[0] * amount, point[1] + direction[1] * amount, point[2] + direction[2] * amount];
}

export function normalizedScreenRayToProject(
  u: number,
  v: number,
  projectionMatrix: ArrayLike<number>,
  worldFromView: ArrayLike<number>,
): ProjectRay | null {
  if (projectionMatrix.length < 16 || worldFromView.length < 16) return null;
  const p00 = Number(projectionMatrix[0]);
  const p11 = Number(projectionMatrix[5]);
  const p20 = Number(projectionMatrix[8]);
  const p21 = Number(projectionMatrix[9]);
  if (![p00, p11, p20, p21].every(Number.isFinite) || Math.abs(p00) < 1e-8 || Math.abs(p11) < 1e-8) return null;
  const ndcX = clamp(u, 0, 1) * 2 - 1;
  const ndcY = 1 - clamp(v, 0, 1) * 2;
  const view: ProjectVector = [(ndcX + p20) / p00, (ndcY + p21) / p11, -1];
  const worldDirection: ProjectVector = [
    Number(worldFromView[0]) * view[0] + Number(worldFromView[4]) * view[1] + Number(worldFromView[8]) * view[2],
    Number(worldFromView[1]) * view[0] + Number(worldFromView[5]) * view[1] + Number(worldFromView[9]) * view[2],
    Number(worldFromView[2]) * view[0] + Number(worldFromView[6]) * view[1] + Number(worldFromView[10]) * view[2],
  ];
  const directionWorld = normalize(worldDirection);
  if (directionWorld === null) return null;
  const origin: ProjectPoint = [
    Number(worldFromView[12]),
    -Number(worldFromView[14]),
    Number(worldFromView[13]),
  ];
  // WorldXR -> project convention [worldX, -worldZ, worldY].
  const direction = normalize([directionWorld[0], -directionWorld[2], directionWorld[1]]);
  return direction === null || !origin.every(Number.isFinite) ? null : { origin, direction };
}

/**
 * Closest-point solve between a forward camera ray and a finite certified edge.
 * The determinant 1-(r·e)^2 is the conditioning term: near-parallel geometry is
 * rejected instead of amplifying small pose/depth errors into a distant point.
 */
export function intersectCameraRayWithCertifiedEdge(
  ray: ProjectRay,
  edge: CertifiedGuideEdge,
  maximumSkewMeters = 0.045,
  endpointToleranceMeters = 0.035,
): RayEdgePlacementResult {
  const edgeVector = subtract(edge.end, edge.start);
  const edgeLength = Math.hypot(edgeVector[0], edgeVector[1], edgeVector[2]);
  const edgeDirection = normalize(edgeVector);
  const rayDirection = normalize(ray.direction);
  if (edgeDirection === null || rayDirection === null || edgeLength < 0.035) {
    return { accepted: false, position: null, edgeId: edge.id, confidence: 0, skewDistanceMeters: null, condition: null, segmentParameter: null, message: 'SCAN MORE · invalid edge geometry.' };
  }

  const w0 = subtract(ray.origin, edge.start);
  const b = dot(rayDirection, edgeDirection);
  const d = dot(rayDirection, w0);
  const e = dot(edgeDirection, w0);
  const determinant = Math.max(0, 1 - b * b);
  if (determinant < 0.035) {
    return { accepted: false, position: null, edgeId: edge.id, confidence: 0, skewDistanceMeters: null, condition: determinant, segmentParameter: null, message: 'SCAN MORE · camera ray is too parallel to this edge.' };
  }

  // Solve min ||(C + t r) - (A + s e)||².
  const rayT = (b * e - d) / determinant;
  const edgeS = (e - b * d) / determinant;
  if (rayT <= 0.03) {
    return { accepted: false, position: null, edgeId: edge.id, confidence: 0, skewDistanceMeters: null, condition: determinant, segmentParameter: edgeS, message: 'SCAN MORE · edge solution is behind the camera.' };
  }
  if (edgeS < -endpointToleranceMeters || edgeS > edgeLength + endpointToleranceMeters) {
    return { accepted: false, position: null, edgeId: edge.id, confidence: 0, skewDistanceMeters: null, condition: determinant, segmentParameter: edgeS, message: 'SCAN MORE · reticle lies outside the measured edge segment.' };
  }

  const clampedS = clamp(edgeS, 0, edgeLength);
  const rayPoint = addScaled(ray.origin, rayDirection, rayT);
  const edgePoint = addScaled(edge.start, edgeDirection, clampedS);
  const skew = Math.hypot(rayPoint[0] - edgePoint[0], rayPoint[1] - edgePoint[1], rayPoint[2] - edgePoint[2]);
  const adaptiveSkew = Math.max(maximumSkewMeters, Math.min(0.07, rayT * 0.012));
  if (skew > adaptiveSkew) {
    return { accepted: false, position: null, edgeId: edge.id, confidence: 0, skewDistanceMeters: skew, condition: determinant, segmentParameter: clampedS, message: 'SCAN MORE · reticle ray misses the certified 3D edge.' };
  }

  const conditioningScore = clamp((determinant - 0.035) / 0.35, 0, 1);
  const skewScore = clamp(1 - skew / Math.max(adaptiveSkew, 1e-6), 0, 1);
  const confidence = Math.min(0.97, edge.confidence * 0.62 + conditioningScore * 0.22 + skewScore * 0.16);
  return {
    accepted: true,
    position: edgePoint,
    edgeId: edge.id,
    confidence,
    skewDistanceMeters: skew,
    condition: determinant,
    segmentParameter: clampedS,
    message: 'EDGE LOCK · camera ray constrained to a certified two-plane edge.',
  };
}
