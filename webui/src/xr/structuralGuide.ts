export type ProjectPoint = readonly [number, number, number];
export type ProjectVector = readonly [number, number, number];

export interface StructuralGuideViewpoint {
  /** Camera position in project coordinates for the current evidence keyframe. */
  position: ProjectPoint;
  /** Optional camera forward vector in project coordinates. */
  forward?: ProjectVector;
  keyframeId: number;
}

export interface StructuralGuideInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  keyframeCount: number;
  voxelSizeMeters: number;
  /** Candidate-specific evidence source. Without a viewpoint, persistence does not mature. */
  viewpoint?: StructuralGuideViewpoint | null;
  /** Conservative ray-verified free-space voxel centers from the occupancy map. */
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
}

export interface StructuralCornerCandidate {
  id: string;
  kind: 'corner';
  position: ProjectPoint;
  confidence: number;
  geometryConfidence: number;
  support: number;
  observations: number;
}

export interface StructuralEdgeCandidate {
  id: string;
  kind: 'edge';
  start: ProjectPoint;
  end: ProjectPoint;
  midpoint: ProjectPoint;
  direction: ProjectVector;
  confidence: number;
  geometryConfidence: number;
  support: number;
  observations: number;
}

export interface StructuralGuideResult {
  corners: readonly StructuralCornerCandidate[];
  edges: readonly StructuralEdgeCandidate[];
  analyzedPoints: number;
  planarPatches: number;
  computeMs: number;
  keyframeCount: number;
  rejectedByFreeSpace: number;
  candidateComparisons: number;
}

interface WeightedPoint {
  point: ProjectPoint;
  inputNormal: ProjectVector;
  confidence: number;
}

interface PlanePatch {
  key: string;
  center: ProjectPoint;
  normal: ProjectVector;
  d: number;
  support: number;
  supportPoints: readonly ProjectPoint[];
  confidence: number;
  planarity: number;
  residualMeters: number;
  ix: number;
  iy: number;
  iz: number;
}

interface RawCorner {
  position: ProjectPoint;
  geometryConfidence: number;
  support: number;
}

interface RawEdge {
  start: ProjectPoint;
  end: ProjectPoint;
  midpoint: ProjectPoint;
  direction: ProjectVector;
  geometryConfidence: number;
  support: number;
}

interface ViewEvidence {
  position: ProjectPoint;
  ray: ProjectVector;
  keyframeId: number;
}

interface TrackedCorner extends RawCorner {
  id: number;
  observations: number;
  lastSeenKeyframe: number;
  evidence: ViewEvidence[];
  innovationEwmaMeters: number;
}

interface TrackedEdge extends RawEdge {
  id: number;
  observations: number;
  lastSeenKeyframe: number;
  evidence: ViewEvidence[];
  innovationEwmaMeters: number;
}

interface GeometryDetectionResult {
  corners: RawCorner[];
  edges: RawEdge[];
  analyzedPoints: number;
  planarPatches: number;
  rejectedByFreeSpace: number;
  candidateComparisons: number;
}

interface SupportInterval {
  min: number;
  max: number;
  count: number;
  meanPerpendicularMeters: number;
}

const PATCH_CELL_METERS = 0.10;
const MIN_POINT_CONFIDENCE = 0.68;
const MIN_PATCH_SUPPORT = 3;
const MAX_PATCH_NEIGHBOUR_METERS = 0.24;
const MIN_PLANE_ANGLE_RAD = (34 * Math.PI) / 180;
const MAX_CORNER_CENTER_DISTANCE_METERS = 0.30;
const MAX_VISIBLE_CORNERS = 8;
const MAX_VISIBLE_EDGES = 10;
const PATCH_RESIDUAL_GOOD_METERS = 0.008;
const PATCH_RESIDUAL_REJECT_METERS = 0.030;
const PATCH_MIN_PLANARITY = 0.54;
const RANSAC_INLIER_METERS = 0.022;
const RANSAC_MAX_TRIALS = 18;
const LOCAL_EDGE_SUPPORT_RADIUS_METERS = 0.075;
const LOCAL_EDGE_SUPPORT_MARGIN_METERS = 0.025;
const MIN_EDGE_LOCAL_SUPPORT_POINTS = 2;
const MIN_EDGE_SUPPORTED_LENGTH_METERS = 0.035;
const MAX_EDGE_SUPPORTED_LENGTH_METERS = 0.34;
const CORNER_LOCAL_SUPPORT_RADIUS_METERS = 0.125;
const MIN_CORNER_LOCAL_SUPPORT_POINTS = 2;
const CORNER_TRACK_RADIUS_METERS = 0.16;
const EDGE_TRACK_RADIUS_METERS = 0.18;
const EDGE_TRACK_MIN_DIRECTION_DOT = Math.cos((24 * Math.PI) / 180);
const EDGE_TRACK_MAX_LATERAL_METERS = 0.075;
const EDGE_TRACK_MIN_OVERLAP_RATIO = 0.20;
const TRACK_SMOOTHING = 0.36;
const TRACK_INITIAL_INNOVATION_METERS = 0.04;
const TRACK_MIN_INNOVATION_GATE_METERS = 0.075;
const STALE_TRACK_KEYFRAMES = 10;
const MIN_VIEW_TRANSLATION_METERS = 0.035;
const MIN_VIEW_BASELINE_METERS = 0.085;
const MIN_VIEW_RAY_ANGLE_RAD = (9 * Math.PI) / 180;
const MAX_TRACKED_VIEW_EVIDENCE = 7;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function finite3(x: number, y: number, z: number): boolean { return Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z); }
function dot(a: ProjectVector, b: ProjectVector): number { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function cross(a: ProjectVector, b: ProjectVector): ProjectVector {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
function length(v: ProjectVector): number { return Math.hypot(v[0], v[1], v[2]); }
function normalize(v: ProjectVector): ProjectVector | null {
  const magnitude = length(v);
  return magnitude > 1e-8 ? [v[0] / magnitude, v[1] / magnitude, v[2] / magnitude] : null;
}
function distance(a: ProjectPoint, b: ProjectPoint): number { return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }
function midpoint(a: ProjectPoint, b: ProjectPoint): ProjectPoint { return [(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5]; }
function addScaled(p: ProjectPoint, v: ProjectVector, scale: number): ProjectPoint { return [p[0] + v[0] * scale, p[1] + v[1] * scale, p[2] + v[2] * scale]; }
function lerpPoint(a: ProjectPoint, b: ProjectPoint, t: number): ProjectPoint {
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
}
function canonicalNormal(normal: ProjectVector): ProjectVector {
  const abs = [Math.abs(normal[0]), Math.abs(normal[1]), Math.abs(normal[2])];
  let axis = 0;
  if (abs[1] > abs[axis]) axis = 1;
  if (abs[2] > abs[axis]) axis = 2;
  return normal[axis] < 0 ? [-normal[0], -normal[1], -normal[2]] : normal;
}
function cellCoord(value: number, size: number): number { return Math.floor(value / size); }
function cellKeyFromCoords(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }
function cellKey(point: ProjectPoint, size: number): string {
  return cellKeyFromCoords(cellCoord(point[0], size), cellCoord(point[1], size), cellCoord(point[2], size));
}
function canonicalDirection(vector: ProjectVector): ProjectVector { return canonicalNormal(vector); }

function symmetricEigen3(matrix: readonly number[]): { values: [number, number, number]; vectors: [ProjectVector, ProjectVector, ProjectVector] } {
  const a = [
    [matrix[0], matrix[1], matrix[2]],
    [matrix[1], matrix[3], matrix[4]],
    [matrix[2], matrix[4], matrix[5]],
  ];
  const v = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  for (let iteration = 0; iteration < 12; iteration += 1) {
    let p = 0; let q = 1; let largest = Math.abs(a[0][1]);
    if (Math.abs(a[0][2]) > largest) { p = 0; q = 2; largest = Math.abs(a[0][2]); }
    if (Math.abs(a[1][2]) > largest) { p = 1; q = 2; largest = Math.abs(a[1][2]); }
    if (largest < 1e-12) break;
    const app = a[p][p]; const aqq = a[q][q]; const apq = a[p][q];
    const phi = 0.5 * Math.atan2(2 * apq, aqq - app); const c = Math.cos(phi); const s = Math.sin(phi);
    for (let k = 0; k < 3; k += 1) {
      if (k === p || k === q) continue;
      const akp = a[k][p]; const akq = a[k][q];
      a[k][p] = a[p][k] = c * akp - s * akq;
      a[k][q] = a[q][k] = s * akp + c * akq;
    }
    a[p][p] = c * c * app - 2 * s * c * apq + s * s * aqq;
    a[q][q] = s * s * app + 2 * s * c * apq + c * c * aqq;
    a[p][q] = a[q][p] = 0;
    for (let k = 0; k < 3; k += 1) {
      const vkp = v[k][p]; const vkq = v[k][q];
      v[k][p] = c * vkp - s * vkq;
      v[k][q] = s * vkp + c * vkq;
    }
  }
  const eigen = [0, 1, 2].map((index) => ({
    value: Math.max(0, a[index][index]),
    vector: normalize([v[0][index], v[1][index], v[2][index]]) ?? [0, 0, 1] as ProjectVector,
  })).sort((left, right) => left.value - right.value);
  return { values: [eigen[0].value, eigen[1].value, eigen[2].value], vectors: [eigen[0].vector, eigen[1].vector, eigen[2].vector] };
}

function weightedPlaneFit(points: readonly WeightedPoint[], extraWeights?: readonly number[]): { center: ProjectPoint; normal: ProjectVector; residual: number; planarity: number; confidence: number } | null {
  if (points.length < MIN_PATCH_SUPPORT) return null;
  let weightSum = 0; let sx = 0; let sy = 0; let sz = 0;
  for (let index = 0; index < points.length; index += 1) {
    const weight = points[index].confidence * (extraWeights?.[index] ?? 1);
    if (weight <= 1e-8) continue;
    weightSum += weight; sx += points[index].point[0] * weight; sy += points[index].point[1] * weight; sz += points[index].point[2] * weight;
  }
  if (weightSum <= 1e-8) return null;
  const center: ProjectPoint = [sx / weightSum, sy / weightSum, sz / weightSum];
  let xx = 0; let xy = 0; let xz = 0; let yy = 0; let yz = 0; let zz = 0;
  let normalX = 0; let normalY = 0; let normalZ = 0; let confidenceSum = 0;
  for (let index = 0; index < points.length; index += 1) {
    const sample = points[index]; const weight = sample.confidence * (extraWeights?.[index] ?? 1);
    if (weight <= 1e-8) continue;
    const dx = sample.point[0] - center[0]; const dy = sample.point[1] - center[1]; const dz = sample.point[2] - center[2];
    xx += weight * dx * dx; xy += weight * dx * dy; xz += weight * dx * dz;
    yy += weight * dy * dy; yz += weight * dy * dz; zz += weight * dz * dz;
    normalX += sample.inputNormal[0] * weight; normalY += sample.inputNormal[1] * weight; normalZ += sample.inputNormal[2] * weight;
    confidenceSum += sample.confidence * weight;
  }
  const invWeight = 1 / weightSum;
  const eigen = symmetricEigen3([xx * invWeight, xy * invWeight, xz * invWeight, yy * invWeight, yz * invWeight, zz * invWeight]);
  let normal = eigen.vectors[0]; const inputNormal = normalize([normalX, normalY, normalZ]);
  if (inputNormal !== null && dot(normal, inputNormal) < 0) normal = [-normal[0], -normal[1], -normal[2]];
  normal = canonicalNormal(normal);
  let squaredResidual = 0; let residualWeight = 0; let normalAgreement = 0;
  for (let index = 0; index < points.length; index += 1) {
    const sample = points[index]; const weight = sample.confidence * (extraWeights?.[index] ?? 1);
    if (weight <= 1e-8) continue;
    const residual = dot(normal, [sample.point[0] - center[0], sample.point[1] - center[1], sample.point[2] - center[2]]);
    squaredResidual += residual * residual * weight; residualWeight += weight; normalAgreement += Math.abs(dot(normal, sample.inputNormal)) * weight;
  }
  const residual = Math.sqrt(squaredResidual / Math.max(1e-9, residualWeight));
  const [lambda0, lambda1, lambda2] = eigen.values;
  const thicknessPlanarity = clamp01(1 - lambda0 / Math.max(lambda1, 1e-9));
  const tangentDimensionality = clamp01((lambda1 / Math.max(lambda2, 1e-9)) / 0.08);
  const planarity = thicknessPlanarity * tangentDimensionality;
  const spread = Math.sqrt(Math.max(0, lambda1 + lambda2));
  const spreadScore = clamp01(spread / 0.028);
  const residualScore = clamp01((PATCH_RESIDUAL_REJECT_METERS - residual) / (PATCH_RESIDUAL_REJECT_METERS - PATCH_RESIDUAL_GOOD_METERS));
  const agreement = normalAgreement / Math.max(1e-9, residualWeight);
  const meanConfidence = confidenceSum / Math.max(1e-9, weightSum);
  const fitConfidence = clamp01(planarity * 0.32 + residualScore * 0.28 + spreadScore * 0.16 + agreement * 0.12 + meanConfidence * 0.12);
  return { center, normal, residual, planarity, confidence: fitConfidence };
}

function robustPlaneFit(points: readonly WeightedPoint[]): { center: ProjectPoint; normal: ProjectVector; residual: number; planarity: number; confidence: number; support: number; supportPoints: readonly ProjectPoint[] } | null {
  let fit = weightedPlaneFit(points); if (fit === null) return null;
  const scale = Math.max(0.006, Math.min(0.028, fit.residual * 2.5));
  const robustWeights = points.map((sample) => {
    const residual = Math.abs(dot(fit!.normal, [sample.point[0] - fit!.center[0], sample.point[1] - fit!.center[1], sample.point[2] - fit!.center[2]]));
    const ratio = residual / scale; return ratio <= 1 ? 1 : 1 / ratio;
  });
  fit = weightedPlaneFit(points, robustWeights) ?? fit;
  if ((fit.planarity < PATCH_MIN_PLANANITY || fit.residual > PATCH_RESIDUAL_REJECT_METERS) && points.length >= 6) {
    let bestInliers: number[] = []; let bestResidual = Number.POSITIVE_INFINITY;
    const trials = Math.min(RANSAC_MAX_TRIALS, points.length * 2);
    for (let trial = 0; trial < trials; trial += 1) {
      const a = trial % points.length; const b = (trial * 3 + 1) % points.length; const c = (trial * 5 + 2) % points.length;
      if (a === b || a === c || b === c) continue;
      const p0 = points[a].point;
      const ab: ProjectVector = [points[b].point[0] - p0[0], points[b].point[1] - p0[1], points[b].point[2] - p0[2]];
      const ac: ProjectVector = [points[c].point[0] - p0[0], points[c].point[1] - p0[1], points[c].point[2] - p0[2]];
      const normal = normalize(cross(ab, ac)); if (normal === null) continue;
      const inliers: number[] = []; let residualSum = 0;
      for (let index = 0; index < points.length; index += 1) {
        const p = points[index].point; const residual = Math.abs(dot(normal, [p[0] - p0[0], p[1] - p0[1], p[2] - p0[2]]));
        if (residual <= RANSAC_INLIER_METERS) { inliers.push(index); residualSum += residual; }
      }
      const average = inliers.length > 0 ? residualSum / inliers.length : Number.POSITIVE_INFINITY;
      if (inliers.length > bestInliers.length || (inliers.length === bestInliers.length && average < bestResidual)) { bestInliers = inliers; bestResidual = average; }
    }
    if (bestInliers.length >= MIN_PATCH_SUPPORT && bestInliers.length >= Math.ceil(points.length * 0.55)) {
      const inlierPoints = bestInliers.map((index) => points[index]); const ransacFit = weightedPlaneFit(inlierPoints);
      if (ransacFit !== null && (ransacFit.confidence > fit.confidence || ransacFit.residual < fit.residual * 0.8)) fit = ransacFit;
    }
  }
  const supportGate = Math.max(RANSAC_INLIER_METERS, Math.min(0.034, fit.residual * 2.5 + 0.008));
  const supportPoints = points
    .filter((sample) => Math.abs(dot(fit!.normal, [sample.point[0] - fit!.center[0], sample.point[1] - fit!.center[1], sample.point[2] - fit!.center[2]])) <= supportGate)
    .map((sample) => sample.point);
  if (supportPoints.length < MIN_PATCH_SUPPORT) return null;
  return { ...fit, support: supportPoints.length, supportPoints };
}

function buildPlanePatches(input: StructuralGuideInput): PlanePatch[] {
  const buckets = new Map<string, { points: WeightedPoint[]; ix: number; iy: number; iz: number }>();
  const count = Math.min(Math.floor(input.positions.length / 3), Math.floor(input.normals.length / 3), input.confidence.length);
  for (let index = 0; index < count; index += 1) {
    const confidence = input.confidence[index]; if (confidence < MIN_POINT_CONFIDENCE) continue;
    const offset = index * 3; const x = input.positions[offset]; const y = input.positions[offset + 1]; const z = input.positions[offset + 2];
    let nx = input.normals[offset]; let ny = input.normals[offset + 1]; let nz = input.normals[offset + 2];
    if (!finite3(x, y, z) || !finite3(nx, ny, nz)) continue;
    const normalized = normalize([nx, ny, nz]); if (normalized === null) continue;
    const oriented = canonicalNormal(normalized); nx = oriented[0]; ny = oriented[1]; nz = oriented[2];
    const ix = cellCoord(x, PATCH_CELL_METERS); const iy = cellCoord(y, PATCH_CELL_METERS); const iz = cellCoord(z, PATCH_CELL_METERS);
    const orientationKey = `${Math.round(nx * 3)}:${Math.round(ny * 3)}:${Math.round(nz * 3)}`;
    const key = `${cellKeyFromCoords(ix, iy, iz)}:${orientationKey}`;
    const bucket = buckets.get(key) ?? { points: [], ix, iy, iz };
    bucket.points.push({ point: [x, y, z], inputNormal: [nx, ny, nz], confidence }); buckets.set(key, bucket);
  }
  const patches: PlanePatch[] = [];
  for (const [key, bucket] of buckets) {
    const fit = robustPlaneFit(bucket.points);
    if (fit === null || fit.support < MIN_PATCH_SUPPORT) continue;
    if (fit.planarity < PATCH_MIN_PLANARITY || fit.residual > PATCH_RESIDUAL_REJECT_METERS || fit.confidence < 0.48) continue;
    patches.push({ key, center: fit.center, normal: fit.normal, d: dot(fit.normal, fit.center), support: fit.support, supportPoints: fit.supportPoints, confidence: fit.confidence, planarity: fit.planarity, residualMeters: fit.residual, ix: bucket.ix, iy: bucket.iy, iz: bucket.iz });
  }
  return patches;
}

function buildFreeSpaceIndex(input: StructuralGuideInput): { size: number; keys: Set<string> } | null {
  const positions = input.freeSpacePositions; const size = input.freeSpaceVoxelSizeMeters ?? 0;
  if (positions === undefined || positions.length < 3 || !Number.isFinite(size) || size <= 0) return null;
  const keys = new Set<string>();
  for (let offset = 0; offset + 2 < positions.length; offset += 3) {
    const point: ProjectPoint = [positions[offset], positions[offset + 1], positions[offset + 2]];
    if (finite3(point[0], point[1], point[2])) keys.add(cellKey(point, size));
  }
  return { size, keys };
}
function isVerifiedFree(index: { size: number; keys: Set<string> } | null, point: ProjectPoint): boolean { return index !== null && index.keys.has(cellKey(point, index.size)); }
function edgeFreeSpaceConflict(index: { size: number; keys: Set<string> } | null, edge: RawEdge): number {
  if (index === null) return 0; let conflicts = 0; const samples = 7;
  for (let sample = 0; sample < samples; sample += 1) if (isVerifiedFree(index, lerpPoint(edge.start, edge.end, sample / (samples - 1)))) conflicts += 1;
  return conflicts / samples;
}

function intersectTwoPlanesNear(a: PlanePatch, b: PlanePatch, anchor: ProjectPoint): { point: ProjectPoint; direction: ProjectVector } | null {
  const c = dot(a.normal, b.normal); const determinant = 1 - c * c; if (determinant < 1e-4) return null;
  const ra = a.d - dot(a.normal, anchor); const rb = b.d - dot(b.normal, anchor);
  const la = (ra - c * rb) / determinant; const lb = (rb - c * ra) / determinant;
  const point: ProjectPoint = [anchor[0] + a.normal[0] * la + b.normal[0] * lb, anchor[1] + a.normal[1] * la + b.normal[1] * lb, anchor[2] + a.normal[2] * la + b.normal[2] * lb];
  const direction = normalize(cross(a.normal, b.normal)); return direction === null ? null : { point, direction: canonicalDirection(direction) };
}
function supportIntervalOnIntersection(patch: PlanePatch, linePoint: ProjectPoint, direction: ProjectVector): SupportInterval | null {
  const parameters: number[] = [];
  let perpendicularSum = 0;
  for (const point of patch.supportPoints) {
    const delta: ProjectVector = [point[0] - linePoint[0], point[1] - linePoint[1], point[2] - linePoint[2]];
    const t = dot(delta, direction);
    const closest = addScaled(linePoint, direction, t);
    const perpendicular = distance(point, closest);
    if (perpendicular > LOCAL_EDGE_SUPPORT_RADIUS_METERS) continue;
    parameters.push(t);
    perpendicularSum += perpendicular;
  }
  const count = parameters.length;
  if (count < MIN_EDGE_LOCAL_SUPPORT_POINTS) return null;
  parameters.sort((a, b) => a - b);
  const trim = count >= 5 ? Math.max(1, Math.floor(count * 0.10)) : 0;
  const low = trim;
  const high = count - 1 - trim;
  if (high <= low) return null;
  return {
    min: parameters[low],
    max: parameters[high],
    count,
    meanPerpendicularMeters: perpendicularSum / count,
  };
}
function supportedEdgeFromPatches(a: PlanePatch, b: PlanePatch, intersection: { point: ProjectPoint; direction: ProjectVector }): RawEdge | null {
  const supportA = supportIntervalOnIntersection(a, intersection.point, intersection.direction);
  const supportB = supportIntervalOnIntersection(b, intersection.point, intersection.direction);
  if (supportA === null || supportB === null) return null;
  let startT = Math.max(supportA.min, supportB.min) - LOCAL_EDGE_SUPPORT_MARGIN_METERS;
  let endT = Math.min(supportA.max, supportB.max) + LOCAL_EDGE_SUPPORT_MARGIN_METERS;
  if (endT - startT < MIN_EDGE_SUPPORTED_LENGTH_METERS) return null;
  if (endT - startT > MAX_EDGE_SUPPORTED_LENGTH_METERS) {
    const center = (startT + endT) * 0.5;
    startT = center - MAX_EDGE_SUPPORTED_LENGTH_METERS * 0.5;
    endT = center + MAX_EDGE_SUPPORTED_LENGTH_METERS * 0.5;
  }
  const start = addScaled(intersection.point, intersection.direction, startT);
  const end = addScaled(intersection.point, intersection.direction, endT);
  const supportQuality = clamp01(1 - (supportA.meanPerpendicularMeters + supportB.meanPerpendicularMeters) / (LOCAL_EDGE_SUPPORT_RADIUS_METERS * 2));
  return {
    start,
    end,
    midpoint: midpoint(start, end),
    direction: intersection.direction,
    geometryConfidence: supportQuality,
    support: supportA.count + supportB.count,
  };
}
function localCornerSupport(patch: PlanePatch, point: ProjectPoint): number {
  let count = 0;
  for (const supportPoint of patch.supportPoints) if (distance(supportPoint, point) <= CORNER_LOCAL_SUPPORT_RADIUS_METERS) count += 1;
  return count;
}
function solveThreePlanes(a: PlanePatch, b: PlanePatch, c: PlanePatch): ProjectPoint | null {
  const bc = cross(b.normal, c.normal); const ca = cross(c.normal, a.normal); const ab = cross(a.normal, b.normal); const determinant = dot(a.normal, bc);
  if (Math.abs(determinant) < 0.12) return null; const inverse = 1 / determinant;
  const point: ProjectPoint = [(a.d * bc[0] + b.d * ca[0] + c.d * ab[0]) * inverse, (a.d * bc[1] + b.d * ca[1] + c.d * ab[1]) * inverse, (a.d * bc[2] + b.d * ca[2] + c.d * ab[2]) * inverse];
  return finite3(point[0], point[1], point[2]) ? point : null;
}
function pairGeometryScore(a: PlanePatch, b: PlanePatch, centerDistance: number): number {
  const absDot = Math.min(1, Math.abs(dot(a.normal, b.normal))); const angle = Math.acos(absDot); const conditioning = clamp01(Math.sin(angle));
  const angleScore = clamp01((angle - MIN_PLANE_ANGLE_RAD) / (Math.PI / 2 - MIN_PLANE_ANGLE_RAD)); const supportScore = clamp01((a.support + b.support) / 18);
  const fitQuality = (a.confidence + b.confidence) * 0.5; const planarity = (a.planarity + b.planarity) * 0.5; const proximity = clamp01(1 - centerDistance / MAX_PATCH_NEIGHBOUR_METERS);
  return clamp01(angleScore * 0.20 + conditioning * 0.16 + supportScore * 0.18 + fitQuality * 0.22 + planarity * 0.14 + proximity * 0.10);
}
function dedupeCorners(raw: RawCorner[]): RawCorner[] {
  const output: RawCorner[] = [];
  for (const candidate of [...raw].sort((a, b) => b.geometryConfidence - a.geometryConfidence)) if (output.find((item) => distance(item.position, candidate.position) <= CORNER_TRACK_RADIUS_METERS * 0.65) === undefined) output.push(candidate);
  return output;
}
function dedupeEdges(raw: RawEdge[]): RawEdge[] {
  const output: RawEdge[] = [];
  for (const candidate of [...raw].sort((a, b) => b.geometryConfidence - a.geometryConfidence)) {
    const existing = output.find((item) => distance(item.midpoint, candidate.midpoint) <= EDGE_TRACK_RADIUS_METERS * 0.65 && Math.abs(dot(item.direction, candidate.direction)) >= EDGE_TRACK_MIN_DIRECTION_DOT);
    if (existing === undefined) output.push(candidate);
  }
  return output;
}

export function detectStructuralGuideGeometry(
  input: StructuralGuideInput,
  includeRawCorners = true,
): GeometryDetectionResult {
  const patches = buildPlanePatches(input); const patchByCell = new Map<string, number[]>();
  for (let index = 0; index < patches.length; index += 1) {
    const patch = patches[index]; const key = cellKeyFromCoords(patch.ix, patch.iy, patch.iz); const bucket = patchByCell.get(key) ?? []; bucket.push(index); patchByCell.set(key, bucket);
  }
  const freeSpace = buildFreeSpaceIndex(input); const edges: RawEdge[] = []; const adjacency = new Map<number, Set<number>>();
  const neighbourCells = Math.ceil(MAX_PATCH_NEIGHBOUR_METERS / PATCH_CELL_METERS); let rejectedByFreeSpace = 0; let candidateComparisons = 0;
  for (let i = 0; i < patches.length; i += 1) {
    const a = patches[i];
    for (let dx = -neighbourCells; dx <= neighbourCells; dx += 1) for (let dy = -neighbourCells; dy <= neighbourCells; dy += 1) for (let dz = -neighbourCells; dz <= neighbourCells; dz += 1) {
      const neighbours = patchByCell.get(cellKeyFromCoords(a.ix + dx, a.iy + dy, a.iz + dz)); if (neighbours === undefined) continue;
      for (const j of neighbours) {
        if (j <= i) continue; candidateComparisons += 1; const b = patches[j]; const centerDistance = distance(a.center, b.center);
        if (centerDistance > MAX_PATCH_NEIGHBOUR_METERS) continue;
        const angle = Math.acos(Math.min(1, Math.abs(dot(a.normal, b.normal)))); if (angle < MIN_PLANE_ANGLE_RAD) continue;
        const intersection = intersectTwoPlanesNear(a, b, midpoint(a.center, b.center)); if (intersection === null) continue;
        if (distance(intersection.point, a.center) > MAX_CORNER_CENTER_DISTANCE_METERS || distance(intersection.point, b.center) > MAX_CORNER_CENTER_DISTANCE_METERS) continue;
        const edge = supportedEdgeFromPatches(a, b, intersection); if (edge === null) continue;
        const geometric = pairGeometryScore(a, b, centerDistance); if (geometric < 0.48) continue;
        edge.geometryConfidence = clamp01(geometric * 0.82 + edge.geometryConfidence * 0.18);
        const freeConflict = edgeFreeSpaceConflict(freeSpace, edge); if (freeConflict >= 0.43) { rejectedByFreeSpace += 1; continue; }
        edge.geometryConfidence *= 1 - freeConflict * 0.72; if (edge.geometryConfidence < 0.48) { rejectedByFreeSpace += 1; continue; }
        edges.push(edge); const forA = adjacency.get(i) ?? new Set<number>(); const forB = adjacency.get(j) ?? new Set<number>(); forA.add(j); forB.add(i); adjacency.set(i, forA); adjacency.set(j, forB);
      }
    }
  }
  if (!includeRawCorners) {
    return {
      corners: [],
      edges: dedupeEdges(edges),
      analyzedPoints: Math.min(Math.floor(input.positions.length / 3), input.confidence.length),
      planarPatches: patches.length,
      rejectedByFreeSpace,
      candidateComparisons,
    };
  }
  const corners: RawCorner[] = [];
  for (const [i, neighboursSet] of adjacency) {
    const neighbours = [...neighboursSet].sort((a, b) => a - b); const a = patches[i];
    for (let x = 0; x < neighbours.length; x += 1) for (let y = x + 1; y < neighbours.length; y += 1) {
      const bIndex = neighbours[x]; const cIndex = neighbours[y]; if (bIndex <= i || cIndex <= i) continue;
      const b = patches[bIndex]; const c = patches[cIndex]; const angleBC = Math.acos(Math.min(1, Math.abs(dot(b.normal, c.normal)))); if (angleBC < MIN_PLANE_ANGLE_RAD) continue;
      const point = solveThreePlanes(a, b, c); if (point === null) continue;
      const maxDistance = Math.max(distance(point, a.center), distance(point, b.center), distance(point, c.center)); if (maxDistance > MAX_CORNER_CENTER_DISTANCE_METERS) continue;
      const localA = localCornerSupport(a, point), localB = localCornerSupport(b, point), localC = localCornerSupport(c, point);
      if (Math.min(localA, localB, localC) < MIN_CORNER_LOCAL_SUPPORT_POINTS) continue;
      if (isVerifiedFree(freeSpace, point)) { rejectedByFreeSpace += 1; continue; }
      const determinant = Math.abs(dot(a.normal, cross(b.normal, c.normal))); const conditioning = clamp01(determinant / 0.78);
      const support = localA + localB + localC;
      const localSupportScore = clamp01(support / 12);
      const fitQuality = (a.confidence + b.confidence + c.confidence) / 3; const planarity = (a.planarity + b.planarity + c.planarity) / 3;
      const score = clamp01(conditioning * 0.28 + localSupportScore * 0.24 + fitQuality * 0.22 + planarity * 0.14 + clamp01(1 - maxDistance / MAX_CORNER_CENTER_DISTANCE_METERS) * 0.12);
      if (score >= 0.48) corners.push({ position: point, geometryConfidence: score, support });
    }
  }
  return { corners: dedupeCorners(corners), edges: dedupeEdges(edges), analyzedPoints: Math.min(Math.floor(input.positions.length / 3), input.confidence.length), planarPatches: patches.length, rejectedByFreeSpace, candidateComparisons };
}

function trackedConfidence(geometry: number, observations: number): number { return clamp01(geometry * 0.68 + clamp01((observations - 1) / 4) * 0.32); }
function observationForCandidate(candidate: ProjectPoint, viewpoint: StructuralGuideViewpoint | null | undefined): ViewEvidence | null {
  if (viewpoint === null || viewpoint === undefined || !Number.isFinite(viewpoint.keyframeId)) return null;
  const ray = normalize([candidate[0] - viewpoint.position[0], candidate[1] - viewpoint.position[1], candidate[2] - viewpoint.position[2]]); if (ray === null) return null;
  if (viewpoint.forward !== undefined) { const forward = normalize(viewpoint.forward); if (forward !== null && dot(forward, ray) <= 0.55) return null; }
  return { position: viewpoint.position, ray, keyframeId: viewpoint.keyframeId };
}
function isIndependentEvidence(evidence: readonly ViewEvidence[], next: ViewEvidence): boolean {
  if (evidence.some((item) => item.keyframeId === next.keyframeId)) return false; if (evidence.length === 0) return true;
  for (const previous of evidence) {
    const baseline = distance(previous.position, next.position);
    if (baseline < MIN_VIEW_TRANSLATION_METERS) return false;
    const rayDot = Math.max(-1, Math.min(1, dot(previous.ray, next.ray))); const angle = Math.acos(rayDot);
    if (baseline < MIN_VIEW_BASELINE_METERS && angle < MIN_VIEW_RAY_ANGLE_RAD) return false;
  }
  return true;
}
function addEvidence(track: { evidence: ViewEvidence[]; observations: number }, candidate: ProjectPoint, viewpoint: StructuralGuideViewpoint | null | undefined): void {
  const next = observationForCandidate(candidate, viewpoint); if (next === null || !isIndependentEvidence(track.evidence, next)) return;
  track.evidence.push(next); if (track.evidence.length > MAX_TRACKED_VIEW_EVIDENCE) track.evidence.shift(); track.observations = Math.min(MAX_TRACKED_VIEW_EVIDENCE, track.observations + 1);
}
function smoothDirection(current: ProjectVector, next: ProjectVector): ProjectVector {
  const aligned: ProjectVector = dot(current, next) < 0 ? [-next[0], -next[1], -next[2]] : next;
  return canonicalDirection(normalize([current[0] + (aligned[0] - current[0]) * TRACK_SMOOTHING, current[1] + (aligned[1] - current[1]) * TRACK_SMOOTHING, current[2] + (aligned[2] - current[2]) * TRACK_SMOOTHING]) ?? current);
}
function trackInnovationGate(track: { innovationEwmaMeters: number; observations: number }, hardMaximum: number): number {
  if (track.observations <= 1) return hardMaximum;
  return Math.min(hardMaximum, Math.max(TRACK_MIN_INNOVATION_GATE_METERS, track.innovationEwmaMeters * 2.5 + 0.025));
}
function updateInnovation(track: { innovationEwmaMeters: number }, innovationMeters: number): void {
  track.innovationEwmaMeters += (innovationMeters - track.innovationEwmaMeters) * 0.28;
}
function edgeLateralDistance(a: RawEdge, b: RawEdge): number {
  const delta: ProjectVector = [b.midpoint[0] - a.midpoint[0], b.midpoint[1] - a.midpoint[1], b.midpoint[2] - a.midpoint[2]];
  const axial = dot(delta, a.direction);
  return Math.hypot(delta[0] - a.direction[0] * axial, delta[1] - a.direction[1] * axial, delta[2] - a.direction[2] * axial);
}
function edgeOverlapRatio(a: RawEdge, b: RawEdge): number {
  const direction = a.direction;
  const a0 = dot(a.start, direction), a1 = dot(a.end, direction);
  const b0 = dot(b.start, direction), b1 = dot(b.end, direction);
  const amin = Math.min(a0, a1), amax = Math.max(a0, a1), bmin = Math.min(b0, b1), bmax = Math.max(b0, b1);
  const overlap = Math.max(0, Math.min(amax, bmax) - Math.max(amin, bmin));
  const smallerLength = Math.max(1e-6, Math.min(amax - amin, bmax - bmin));
  return clamp01(overlap / smallerLength);
}

export class StructuralGuideTracker {
  private corners = new Map<number, TrackedCorner>(); private edges = new Map<number, TrackedEdge>(); private nextCornerId = 1; private nextEdgeId = 1;
  reset(): void { this.corners.clear(); this.edges.clear(); this.nextCornerId = 1; this.nextEdgeId = 1; }
  update(input: StructuralGuideInput, computeMs = 0, includeRawCorners = true): StructuralGuideResult {
    const raw = detectStructuralGuideGeometry(input, includeRawCorners); const matchedCorners = new Set<number>(); const matchedEdges = new Set<number>();
    if (!includeRawCorners && this.corners.size > 0) this.corners.clear();
    for (const candidate of raw.corners) {
      let best: TrackedCorner | null = null; let bestDistance = Number.POSITIVE_INFINITY;
      for (const track of this.corners.values()) {
        if (matchedCorners.has(track.id)) continue;
        const candidateDistance = distance(track.position, candidate.position);
        if (candidateDistance > trackInnovationGate(track, CORNER_TRACK_RADIUS_METERS) || candidateDistance >= bestDistance) continue;
        best = track; bestDistance = candidateDistance;
      }
      if (best === null) {
        const id = this.nextCornerId++;
        const created: TrackedCorner = { ...candidate, id, observations: 0, lastSeenKeyframe: input.keyframeCount, evidence: [], innovationEwmaMeters: TRACK_INITIAL_INNOVATION_METERS };
        addEvidence(created, candidate.position, input.viewpoint); this.corners.set(id, created); matchedCorners.add(id);
      } else {
        matchedCorners.add(best.id); updateInnovation(best, bestDistance);
        best.position = lerpPoint(best.position, candidate.position, TRACK_SMOOTHING);
        best.geometryConfidence += (candidate.geometryConfidence - best.geometryConfidence) * TRACK_SMOOTHING;
        best.support = Math.max(best.support, candidate.support); best.lastSeenKeyframe = input.keyframeCount;
        addEvidence(best, candidate.position, input.viewpoint);
      }
    }
    for (const candidate of raw.edges) {
      let best: TrackedEdge | null = null; let bestScore = Number.POSITIVE_INFINITY; let bestDistance = 0;
      for (const track of this.edges.values()) {
        if (matchedEdges.has(track.id)) continue;
        const directionDot = Math.abs(dot(track.direction, candidate.direction)); if (directionDot < EDGE_TRACK_MIN_DIRECTION_DOT) continue;
        const candidateDistance = distance(track.midpoint, candidate.midpoint);
        if (candidateDistance > trackInnovationGate(track, EDGE_TRACK_RADIUS_METERS)) continue;
        const lateral = edgeLateralDistance(track, candidate); if (lateral > EDGE_TRACK_MAX_LATERAL_METERS) continue;
        const overlap = edgeOverlapRatio(track, candidate); if (overlap < EDGE_TRACK_MIN_OVERLAP_RATIO) continue;
        const score = candidateDistance + lateral * 0.8 + (1 - overlap) * 0.05 + (1 - directionDot) * 0.08;
        if (score >= bestScore) continue;
        best = track; bestScore = score; bestDistance = candidateDistance;
      }
      if (best === null) {
        const id = this.nextEdgeId++;
        const created: TrackedEdge = { ...candidate, id, observations: 0, lastSeenKeyframe: input.keyframeCount, evidence: [], innovationEwmaMeters: TRACK_INITIAL_INNOVATION_METERS };
        addEvidence(created, candidate.midpoint, input.viewpoint); this.edges.set(id, created); matchedEdges.add(id);
      } else {
        matchedEdges.add(best.id); updateInnovation(best, bestDistance);
        best.start = lerpPoint(best.start, candidate.start, TRACK_SMOOTHING); best.end = lerpPoint(best.end, candidate.end, TRACK_SMOOTHING);
        best.midpoint = lerpPoint(best.midpoint, candidate.midpoint, TRACK_SMOOTHING); best.direction = smoothDirection(best.direction, candidate.direction);
        best.geometryConfidence += (candidate.geometryConfidence - best.geometryConfidence) * TRACK_SMOOTHING;
        best.support = Math.max(best.support, candidate.support); best.lastSeenKeyframe = input.keyframeCount;
        addEvidence(best, candidate.midpoint, input.viewpoint);
      }
    }
    for (const [id, track] of this.corners) if (!matchedCorners.has(id) && input.keyframeCount - track.lastSeenKeyframe > STALE_TRACK_KEYFRAMES) this.corners.delete(id);
    for (const [id, track] of this.edges) if (!matchedEdges.has(id) && input.keyframeCount - track.lastSeenKeyframe > STALE_TRACK_KEYFRAMES) this.edges.delete(id);
    const corners: StructuralCornerCandidate[] = [];
    for (const track of this.corners.values()) { const confidence = trackedConfidence(track.geometryConfidence, track.observations); if (track.observations >= 2 && confidence >= 0.58) corners.push({ id: `corner:c${track.id}`, kind: 'corner', position: track.position, confidence, geometryConfidence: track.geometryConfidence, support: track.support, observations: track.observations }); }
    const edges: StructuralEdgeCandidate[] = [];
    for (const track of this.edges.values()) { const confidence = trackedConfidence(track.geometryConfidence, track.observations); if (track.observations >= 2 && confidence >= 0.58) edges.push({ id: `edge:e${track.id}`, kind: 'edge', start: track.start, end: track.end, midpoint: track.midpoint, direction: track.direction, confidence, geometryConfidence: track.geometryConfidence, support: track.support, observations: track.observations }); }
    corners.sort((a, b) => b.confidence - a.confidence); edges.sort((a, b) => b.confidence - a.confidence);
    return { corners: corners.slice(0, MAX_VISIBLE_CORNERS), edges: edges.slice(0, MAX_VISIBLE_EDGES), analyzedPoints: raw.analyzedPoints, planarPatches: raw.planarPatches, computeMs, keyframeCount: input.keyframeCount, rejectedByFreeSpace: raw.rejectedByFreeSpace, candidateComparisons: raw.candidateComparisons };
  }
}

export function distancePointToSegment(point: ProjectPoint, start: ProjectPoint, end: ProjectPoint): number {
  const dx = end[0] - start[0]; const dy = end[1] - start[1]; const dz = end[2] - start[2]; const denominator = dx * dx + dy * dy + dz * dz;
  if (denominator < 1e-9) return distance(point, start);
  const t = clamp01(((point[0] - start[0]) * dx + (point[1] - start[1]) * dy + (point[2] - start[2]) * dz) / denominator);
  return Math.hypot(point[0] - (start[0] + dx * t), point[1] - (start[1] + dy * t), point[2] - (start[2] + dz * t));
}
export interface ManualPointLike { id: number; position: ProjectPoint; }
export function matchEdgeToManualPoints(edge: StructuralEdgeCandidate, points: readonly ManualPointLike[], radiusMeters = 0.14): readonly [ManualPointLike, ManualPointLike] | null {
  if (points.length < 2) return null; let best: null | { a: ManualPointLike; b: ManualPointLike; score: number } | null = null;
  for (let i = 0; i < points.length; i += 1) for (let j = i + 1; j < points.length; j += 1) {
    const p = points[i]; const q = points[j]; const direct = distance(p.position, edge.start) + distance(q.position, edge.end); const reverse = distance(p.position, edge.end) + distance(q.position, edge.start); const score = Math.min(direct, reverse);
    if (score > radiusMeters * 2 || distancePointToSegment(midpoint(p.position, q.position), edge.start, edge.end) > radiusMeters) continue;
    if (best === null || score < best.score) best = direct <= reverse ? { a: p, b: q, score } : { a: q, b: p, score };
  }
  return best === null ? null : [best.a, best.b];
}

export const structuralGuideMathForTests = { symmetricEigen3, isIndependentEvidence };
