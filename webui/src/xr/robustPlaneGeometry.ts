export type Point3 = readonly [number, number, number];
export type Vector3 = readonly [number, number, number];

export interface RobustPlaneSample {
  position: Point3;
  normal?: Vector3 | null;
  weight: number;
  residualMeters?: number;
  normalConfidence?: number;
}

export interface RobustPlane {
  center: Point3;
  normal: Vector3;
  d: number;
  support: number;
  supportWeight: number;
  rmsResidualMeters: number;
  robustScaleMeters: number;
  /** Minimum standard deviation across the two tangent PCA axes. */
  tangentSpreadMeters: number;
  offsetSigmaMeters: number;
  confidence: number;
  inlierIndices: readonly number[];
}

export interface EdgeIntersectionEstimate {
  point: Point3;
  direction: Vector3;
  angleRadians: number;
  uncertaintyMeters: number;
  confidence: number;
}

export interface CornerIntersectionEstimate {
  position: Point3;
  conditionNumber: number;
  maxStdDevMeters: number;
  confidence: number;
}

export interface PlaneExtractionOptions {
  maxPlanes?: number;
  minSupport?: number;
  inlierThresholdMeters?: number;
  normalGateRadians?: number;
  maxTrials?: number;
  minimumTangentSpreadMeters?: number;
}

const DEFAULT_MIN_SUPPORT = 6;
const DEFAULT_INLIER_METERS = 0.018;
const DEFAULT_NORMAL_GATE_RADIANS = (38 * Math.PI) / 180;
const DEFAULT_MAX_TRIALS = 72;
const DEFAULT_MIN_TANGENT_SPREAD_METERS = 0.018;
const HUBER_K = 1.5;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

export function dot3(a: Vector3, b: Vector3): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

export function cross3(a: Vector3, b: Vector3): Vector3 {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

export function length3(vector: Vector3): number {
  return Math.hypot(vector[0], vector[1], vector[2]);
}

export function normalize3(vector: Vector3): Vector3 | null {
  const magnitude = length3(vector);
  if (!Number.isFinite(magnitude) || magnitude < 1e-9) return null;
  return [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude];
}

export function distance3(a: Point3, b: Point3): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function subtract3(a: Point3, b: Point3): Vector3 {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function addScaled3(point: Point3, vector: Vector3, scale: number): Point3 {
  return [
    point[0] + vector[0] * scale,
    point[1] + vector[1] * scale,
    point[2] + vector[2] * scale,
  ];
}

function canonicalNormal(normal: Vector3): Vector3 {
  const ax = Math.abs(normal[0]);
  const ay = Math.abs(normal[1]);
  const az = Math.abs(normal[2]);
  const axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
  return normal[axis] < 0 ? [-normal[0], -normal[1], -normal[2]] : normal;
}

function median(values: readonly number[]): number {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0
    ? (sorted[middle - 1] + sorted[middle]) * 0.5
    : sorted[middle];
}

function robustScale(residuals: readonly number[]): number {
  if (residuals.length === 0) return 0;
  const centre = median(residuals);
  const deviations = residuals.map((value) => Math.abs(value - centre));
  // 1.4826 * MAD is a Gaussian-consistent robust scale estimate.
  return Math.max(0.0015, 1.4826 * median(deviations));
}

/** Jacobi eigensolver for a real symmetric 3x3 matrix packed as xx,xy,xz,yy,yz,zz. */
export function symmetricEigen3(
  matrix: readonly number[],
): { values: [number, number, number]; vectors: [Vector3, Vector3, Vector3] } {
  const a = [
    [matrix[0], matrix[1], matrix[2]],
    [matrix[1], matrix[3], matrix[4]],
    [matrix[2], matrix[4], matrix[5]],
  ];
  const v = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  for (let iteration = 0; iteration < 18; iteration += 1) {
    let p = 0;
    let q = 1;
    let largest = Math.abs(a[0][1]);
    if (Math.abs(a[0][2]) > largest) { p = 0; q = 2; largest = Math.abs(a[0][2]); }
    if (Math.abs(a[1][2]) > largest) { p = 1; q = 2; largest = Math.abs(a[1][2]); }
    if (largest < 1e-13) break;

    const app = a[p][p];
    const aqq = a[q][q];
    const apq = a[p][q];
    const phi = 0.5 * Math.atan2(2 * apq, aqq - app);
    const c = Math.cos(phi);
    const s = Math.sin(phi);
    for (let k = 0; k < 3; k += 1) {
      if (k === p || k === q) continue;
      const akp = a[k][p];
      const akq = a[k][q];
      a[k][p] = a[p][k] = c * akp - s * akq;
      a[k][q] = a[q][k] = s * akp + c * akq;
    }
    a[p][p] = c * c * app - 2 * s * c * apq + s * s * aqq;
    a[q][q] = s * s * app + 2 * s * c * apq + c * c * aqq;
    a[p][q] = a[q][p] = 0;
    for (let k = 0; k < 3; k += 1) {
      const vkp = v[k][p];
      const vkq = v[k][q];
      v[k][p] = c * vkp - s * vkq;
      v[k][q] = s * vkp + c * vkq;
    }
  }

  const eigen = [0, 1, 2]
    .map((index) => ({
      value: Math.max(0, a[index][index]),
      vector: normalize3([v[0][index], v[1][index], v[2][index]]) ?? [0, 0, 1] as Vector3,
    }))
    .sort((left, right) => left.value - right.value);
  return {
    values: [eigen[0].value, eigen[1].value, eigen[2].value],
    vectors: [eigen[0].vector, eigen[1].vector, eigen[2].vector],
  };
}

function planeFromThree(a: Point3, b: Point3, c: Point3): { normal: Vector3; d: number } | null {
  const normal = normalize3(cross3(subtract3(b, a), subtract3(c, a)));
  if (normal === null) return null;
  const canonical = canonicalNormal(normal);
  return { normal: canonical, d: dot3(canonical, a) };
}

function sampleWeight(sample: RobustPlaneSample): number {
  const base = clamp(sample.weight, 0.01, 1);
  const normal = clamp(sample.normalConfidence ?? 0.7, 0.1, 1);
  const residual = Math.max(0, sample.residualMeters ?? 0);
  const residualPenalty = 1 / (1 + (residual / 0.025) ** 2);
  return base * (0.55 + normal * 0.45) * residualPenalty;
}

function fitWeightedTls(
  samples: readonly RobustPlaneSample[],
  indices: readonly number[],
  extraWeights?: readonly number[],
): RobustPlane | null {
  if (indices.length < 3) return null;
  let totalWeight = 0;
  let cx = 0;
  let cy = 0;
  let cz = 0;
  for (let localIndex = 0; localIndex < indices.length; localIndex += 1) {
    const sample = samples[indices[localIndex]];
    const weight = sampleWeight(sample) * (extraWeights?.[localIndex] ?? 1);
    if (weight <= 1e-8) continue;
    totalWeight += weight;
    cx += sample.position[0] * weight;
    cy += sample.position[1] * weight;
    cz += sample.position[2] * weight;
  }
  if (totalWeight <= 1e-8) return null;
  const center: Point3 = [cx / totalWeight, cy / totalWeight, cz / totalWeight];

  let xx = 0; let xy = 0; let xz = 0; let yy = 0; let yz = 0; let zz = 0;
  let suppliedNx = 0; let suppliedNy = 0; let suppliedNz = 0; let suppliedNormalWeight = 0;
  for (let localIndex = 0; localIndex < indices.length; localIndex += 1) {
    const sample = samples[indices[localIndex]];
    const weight = sampleWeight(sample) * (extraWeights?.[localIndex] ?? 1);
    if (weight <= 1e-8) continue;
    const dx = sample.position[0] - center[0];
    const dy = sample.position[1] - center[1];
    const dz = sample.position[2] - center[2];
    xx += weight * dx * dx; xy += weight * dx * dy; xz += weight * dx * dz;
    yy += weight * dy * dy; yz += weight * dy * dz; zz += weight * dz * dz;
    if (sample.normal !== undefined && sample.normal !== null) {
      const n = normalize3(sample.normal);
      if (n !== null) {
        const sign = suppliedNormalWeight > 0 && suppliedNx * n[0] + suppliedNy * n[1] + suppliedNz * n[2] < 0 ? -1 : 1;
        const nw = weight * clamp(sample.normalConfidence ?? 0.7, 0.1, 1);
        suppliedNx += n[0] * nw * sign;
        suppliedNy += n[1] * nw * sign;
        suppliedNz += n[2] * nw * sign;
        suppliedNormalWeight += nw;
      }
    }
  }

  const inverseWeight = 1 / totalWeight;
  const eigen = symmetricEigen3([
    xx * inverseWeight,
    xy * inverseWeight,
    xz * inverseWeight,
    yy * inverseWeight,
    yz * inverseWeight,
    zz * inverseWeight,
  ]);
  let normal = eigen.vectors[0];
  const supplied = suppliedNormalWeight > 0
    ? normalize3([suppliedNx, suppliedNy, suppliedNz])
    : null;
  if (supplied !== null && dot3(normal, supplied) < 0) normal = [-normal[0], -normal[1], -normal[2]];
  normal = canonicalNormal(normal);
  const d = dot3(normal, center);

  const residuals = indices.map((index) => Math.abs(dot3(normal, samples[index].position) - d));
  let weightedSquaredResidual = 0;
  let residualWeight = 0;
  for (let localIndex = 0; localIndex < indices.length; localIndex += 1) {
    const weight = sampleWeight(samples[indices[localIndex]]) * (extraWeights?.[localIndex] ?? 1);
    weightedSquaredResidual += residuals[localIndex] * residuals[localIndex] * weight;
    residualWeight += weight;
  }
  const rmsResidualMeters = Math.sqrt(weightedSquaredResidual / Math.max(1e-9, residualWeight));
  const scaleMeters = robustScale(residuals);
  // λ0 is the fitted-plane normal variance. A true 2-D surface must also have
  // measurable support along BOTH tangent axes. Using sqrt(λ1+λ2) lets a long,
  // almost one-dimensional line pass as a plane because λ2 alone can dominate.
  // The smaller tangent axis is therefore the conservative plane-width metric.
  const tangentSpreadMeters = Math.sqrt(Math.max(0, eigen.values[1]));
  // Offset uncertainty combines measured scatter and per-surfel accumulated residuals.
  const measurementSigma = Math.max(
    scaleMeters,
    Math.sqrt(indices.reduce((sum, index) => sum + (samples[index].residualMeters ?? 0) ** 2, 0) / indices.length),
    0.0025,
  );
  const offsetSigmaMeters = measurementSigma / Math.sqrt(Math.max(1, effectiveSampleSize(samples, indices)));
  const residualScore = clamp01(1 - rmsResidualMeters / 0.028);
  const supportScore = clamp01((indices.length - 3) / 9);
  const spreadScore = clamp01(tangentSpreadMeters / 0.055);
  const normalScore = suppliedNormalWeight > 0
    ? clamp01(Math.abs(dot3(normal, supplied ?? normal)))
    : 0.65;
  const confidence = clamp01(
    residualScore * 0.38 + supportScore * 0.24 + spreadScore * 0.20 + normalScore * 0.18,
  );

  return {
    center,
    normal,
    d,
    support: indices.length,
    supportWeight: totalWeight,
    rmsResidualMeters,
    robustScaleMeters: scaleMeters,
    tangentSpreadMeters,
    offsetSigmaMeters,
    confidence,
    inlierIndices: [...indices],
  };
}

function effectiveSampleSize(samples: readonly RobustPlaneSample[], indices: readonly number[]): number {
  let sum = 0;
  let squared = 0;
  for (const index of indices) {
    const weight = sampleWeight(samples[index]);
    sum += weight;
    squared += weight * weight;
  }
  return squared > 1e-9 ? (sum * sum) / squared : 1;
}

function irlsPlane(
  samples: readonly RobustPlaneSample[],
  indices: readonly number[],
): RobustPlane | null {
  let fit = fitWeightedTls(samples, indices);
  if (fit === null) return null;
  for (let iteration = 0; iteration < 4; iteration += 1) {
    const residuals = indices.map((index) => Math.abs(dot3(fit!.normal, samples[index].position) - fit!.d));
    const scale = Math.max(0.0015, robustScale(residuals));
    const huberLimit = HUBER_K * scale;
    const robustWeights = residuals.map((residual) => residual <= huberLimit ? 1 : huberLimit / Math.max(residual, 1e-9));
    const next = fitWeightedTls(samples, indices, robustWeights);
    if (next === null) break;
    fit = next;
  }
  return fit;
}

function deterministicTriplet(trial: number, count: number): readonly [number, number, number] | null {
  if (count < 3) return null;
  const a = trial % count;
  let b = (trial * 7 + 3) % count;
  let c = (trial * 13 + 5) % count;
  if (b === a) b = (b + 1) % count;
  if (c === a || c === b) c = (c + 2) % count;
  if (a === b || a === c || b === c) return null;
  return [a, b, c];
}

function hypothesisInliers(
  samples: readonly RobustPlaneSample[],
  active: readonly number[],
  plane: { normal: Vector3; d: number },
  distanceGate: number,
  normalGateCosine: number,
): number[] {
  const inliers: number[] = [];
  for (const index of active) {
    const sample = samples[index];
    const residual = Math.abs(dot3(plane.normal, sample.position) - plane.d);
    const adaptiveGate = Math.max(distanceGate, (sample.residualMeters ?? 0) * 2.2 + 0.004);
    if (residual > adaptiveGate) continue;
    if (sample.normal !== undefined && sample.normal !== null && (sample.normalConfidence ?? 0) >= 0.35) {
      const n = normalize3(sample.normal);
      if (n !== null && Math.abs(dot3(n, plane.normal)) < normalGateCosine) continue;
    }
    inliers.push(index);
  }
  return inliers;
}

/**
 * Extract locally dominant planes using deterministic RANSAC followed by
 * weighted total-least-squares and Huber IRLS refinement.
 *
 * This deliberately estimates each plane from 3D positions. Supplied surfel
 * normals are only a compatibility/weight cue; they are never averaged and
 * mistaken for a plane fit.
 */
export function extractRobustPlanes(
  samples: readonly RobustPlaneSample[],
  options: PlaneExtractionOptions = {},
): RobustPlane[] {
  const maxPlanes = clamp(Math.floor(options.maxPlanes ?? 3), 1, 6);
  const minSupport = Math.max(3, Math.floor(options.minSupport ?? DEFAULT_MIN_SUPPORT));
  const inlierThresholdMeters = clamp(options.inlierThresholdMeters ?? DEFAULT_INLIER_METERS, 0.004, 0.05);
  const normalGateRadians = clamp(options.normalGateRadians ?? DEFAULT_NORMAL_GATE_RADIANS, (10 * Math.PI) / 180, Math.PI / 2);
  const normalGateCosine = Math.cos(normalGateRadians);
  const maxTrials = Math.max(12, Math.floor(options.maxTrials ?? DEFAULT_MAX_TRIALS));
  const minimumTangentSpreadMeters = options.minimumTangentSpreadMeters ?? DEFAULT_MIN_TANGENT_SPREAD_METERS;
  let active = samples
    .map((sample, index) => ({ sample, index }))
    .filter(({ sample }) => Number.isFinite(sample.weight) && sample.weight > 0)
    .map(({ index }) => index);
  const planes: RobustPlane[] = [];

  while (planes.length < maxPlanes && active.length >= minSupport) {
    let bestInliers: number[] = [];
    let bestScore = Number.NEGATIVE_INFINITY;
    const trials = Math.min(maxTrials, Math.max(12, active.length * 3));
    for (let trial = 0; trial < trials; trial += 1) {
      const triplet = deterministicTriplet(trial, active.length);
      if (triplet === null) continue;
      const hypothesis = planeFromThree(
        samples[active[triplet[0]]].position,
        samples[active[triplet[1]]].position,
        samples[active[triplet[2]]].position,
      );
      if (hypothesis === null) continue;
      const inliers = hypothesisInliers(
        samples,
        active,
        hypothesis,
        inlierThresholdMeters,
        normalGateCosine,
      );
      if (inliers.length < minSupport) continue;
      const supportWeight = inliers.reduce((sum, index) => sum + sampleWeight(samples[index]), 0);
      const residual = inliers.reduce(
        (sum, index) => sum + Math.abs(dot3(hypothesis.normal, samples[index].position) - hypothesis.d),
        0,
      ) / inliers.length;
      const score = supportWeight - residual * 18;
      if (score > bestScore) {
        bestScore = score;
        bestInliers = inliers;
      }
    }

    if (bestInliers.length < minSupport) break;
    const fit = irlsPlane(samples, bestInliers);
    if (
      fit === null ||
      fit.support < minSupport ||
      fit.rmsResidualMeters > Math.max(0.028, inlierThresholdMeters * 1.5) ||
      fit.tangentSpreadMeters < minimumTangentSpreadMeters ||
      fit.confidence < 0.50
    ) break;

    // Recompute inliers around the refined plane, then refine once more.
    const refinedInliers = hypothesisInliers(
      samples,
      active,
      fit,
      inlierThresholdMeters,
      normalGateCosine,
    );
    const refined = refinedInliers.length >= minSupport
      ? irlsPlane(samples, refinedInliers) ?? fit
      : fit;
    planes.push(refined);
    const used = new Set(refined.inlierIndices);
    active = active.filter((index) => !used.has(index));
  }

  return planes.sort((left, right) => {
    const leftScore = left.supportWeight * left.confidence;
    const rightScore = right.supportWeight * right.confidence;
    return rightScore - leftScore;
  });
}

export function intersectTwoRobustPlanes(
  first: RobustPlane,
  second: RobustPlane,
  target: Point3,
): EdgeIntersectionEstimate | null {
  const cosine = clamp(Math.abs(dot3(first.normal, second.normal)), 0, 1);
  const angleRadians = Math.acos(cosine);
  const sine = Math.sqrt(Math.max(0, 1 - cosine * cosine));
  if (sine < Math.sin((24 * Math.PI) / 180)) return null;

  const c = dot3(first.normal, second.normal);
  const determinant = 1 - c * c;
  if (determinant < 1e-5) return null;
  const ra = first.d - dot3(first.normal, target);
  const rb = second.d - dot3(second.normal, target);
  const la = (ra - c * rb) / determinant;
  const lb = (rb - c * ra) / determinant;
  const point: Point3 = [
    target[0] + first.normal[0] * la + second.normal[0] * lb,
    target[1] + first.normal[1] * la + second.normal[1] * lb,
    target[2] + first.normal[2] * la + second.normal[2] * lb,
  ];
  const direction = normalize3(cross3(first.normal, second.normal));
  if (direction === null) return null;
  const uncertaintyMeters = Math.sqrt(
    first.offsetSigmaMeters ** 2 + second.offsetSigmaMeters ** 2,
  ) / Math.max(sine, 0.08);
  const confidence = clamp01(
    Math.min(first.confidence, second.confidence) *
    clamp01((angleRadians - (24 * Math.PI) / 180) / ((65 * Math.PI) / 180)) * 0.45 +
    Math.min(first.confidence, second.confidence) * 0.55,
  );
  return { point, direction, angleRadians, uncertaintyMeters, confidence };
}

function invertSymmetric3(matrix: readonly number[]): readonly number[] | null {
  const [a, b, c, d, e, f] = matrix;
  const cofactor00 = d * f - e * e;
  const cofactor01 = c * e - b * f;
  const cofactor02 = b * e - c * d;
  const cofactor11 = a * f - c * c;
  const cofactor12 = b * c - a * e;
  const cofactor22 = a * d - b * b;
  const determinant = a * cofactor00 + b * cofactor01 + c * cofactor02;
  if (!Number.isFinite(determinant) || Math.abs(determinant) < 1e-12) return null;
  const inv = 1 / determinant;
  return [
    cofactor00 * inv,
    cofactor01 * inv,
    cofactor02 * inv,
    cofactor11 * inv,
    cofactor12 * inv,
    cofactor22 * inv,
  ];
}

/** Weighted least-squares three-plane intersection with covariance/conditioning. */
export function solveThreeRobustPlanes(
  first: RobustPlane,
  second: RobustPlane,
  third: RobustPlane,
): CornerIntersectionEstimate | null {
  const planes = [first, second, third] as const;
  let xx = 0; let xy = 0; let xz = 0; let yy = 0; let yz = 0; let zz = 0;
  let bx = 0; let by = 0; let bz = 0;
  for (const plane of planes) {
    const sigma = clamp(plane.offsetSigmaMeters, 0.0025, 0.05);
    // Information is inverse variance scaled by the physical plane-fit confidence.
    // The previous Math.max(1, confidence) made every 0..1 confidence exactly 1,
    // so a weak plane contributed the same covariance information as a strong one.
    const confidenceWeight = clamp(plane.confidence, 0.05, 1);
    const weight = confidenceWeight / (sigma * sigma);
    const [nx, ny, nz] = plane.normal;
    xx += weight * nx * nx; xy += weight * nx * ny; xz += weight * nx * nz;
    yy += weight * ny * ny; yz += weight * ny * nz; zz += weight * nz * nz;
    bx += weight * plane.d * nx;
    by += weight * plane.d * ny;
    bz += weight * plane.d * nz;
  }
  const information = [xx, xy, xz, yy, yz, zz] as const;
  const eigen = symmetricEigen3(information);
  const minimum = Math.max(eigen.values[0], 1e-12);
  const maximum = Math.max(eigen.values[2], minimum);
  const conditionNumber = maximum / minimum;
  if (!Number.isFinite(conditionNumber) || conditionNumber > 45) return null;
  const inverse = invertSymmetric3(information);
  if (inverse === null) return null;
  const position: Point3 = [
    inverse[0] * bx + inverse[1] * by + inverse[2] * bz,
    inverse[1] * bx + inverse[3] * by + inverse[4] * bz,
    inverse[2] * bx + inverse[4] * by + inverse[5] * bz,
  ];
  if (!position.every(Number.isFinite)) return null;
  // Largest covariance eigenvalue = inverse of smallest information eigenvalue.
  const maxStdDevMeters = Math.sqrt(1 / minimum);
  const conditioningScore = clamp01((45 - conditionNumber) / 38);
  const uncertaintyScore = clamp01((0.040 - maxStdDevMeters) / 0.034);
  const confidence = clamp01(
    Math.min(first.confidence, second.confidence, third.confidence) * 0.48 +
    conditioningScore * 0.27 +
    uncertaintyScore * 0.25,
  );
  return { position, conditionNumber, maxStdDevMeters, confidence };
}

export function closestPointsBetweenLines(
  firstOrigin: Point3,
  firstDirection: Vector3,
  secondOrigin: Point3,
  secondDirection: Vector3,
): { first: Point3; second: Point3; separationMeters: number } | null {
  const u = normalize3(firstDirection);
  const v = normalize3(secondDirection);
  if (u === null || v === null) return null;
  const w0 = subtract3(firstOrigin, secondOrigin);
  const a = dot3(u, u);
  const b = dot3(u, v);
  const c = dot3(v, v);
  const d = dot3(u, w0);
  const e = dot3(v, w0);
  const denominator = a * c - b * b;
  if (Math.abs(denominator) < 1e-7) return null;
  const firstT = (b * e - c * d) / denominator;
  const secondT = (a * e - b * d) / denominator;
  const first = addScaled3(firstOrigin, u, firstT);
  const second = addScaled3(secondOrigin, v, secondT);
  return { first, second, separationMeters: distance3(first, second) };
}
