export interface SurfaceNormalEvidence {
  normalX: number;
  normalY: number;
  normalZ: number;
  normalConfidence: number;
}

export interface SurfaceFeatureComplexity {
  score: number;
  matureOrientationFamilies: number;
  robustSpreadRadians: number;
  strongestFamilySeparationRadians: number;
  usableNormals: number;
}

interface OrientationFamily {
  x: number;
  y: number;
  z: number;
  weight: number;
  members: number;
}

const MIN_NORMAL_CONFIDENCE = 0.35;
const FAMILY_ANGLE_RADIANS = (24 * Math.PI) / 180;
const FAMILY_DOT = Math.cos(FAMILY_ANGLE_RADIANS);
const MIN_MATURE_FAMILY_MEMBERS = 2;
const CURVATURE_START_RADIANS = (9 * Math.PI) / 180;
const CURVATURE_FULL_RADIANS = (34 * Math.PI) / 180;
const FAMILY_SEPARATION_FULL_RADIANS = (65 * Math.PI) / 180;
export const MAX_FEATURE_POINTS_PER_COVERAGE_CELL = 48;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function normalize(x: number, y: number, z: number): readonly [number, number, number] | null {
  const length = Math.hypot(x, y, z);
  return Number.isFinite(length) && length > 1e-7 ? [x / length, y / length, z / length] : null;
}
function absDot(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return Math.abs(Math.max(-1, Math.min(1, a[0] * b[0] + a[1] * b[1] + a[2] * b[2])));
}
function signAligned(
  normal: readonly [number, number, number],
  reference: readonly [number, number, number],
): readonly [number, number, number] {
  return normal[0] * reference[0] + normal[1] * reference[1] + normal[2] * reference[2] < 0
    ? [-normal[0], -normal[1], -normal[2]]
    : normal;
}
function familyNormal(family: OrientationFamily): readonly [number, number, number] | null {
  return normalize(family.x, family.y, family.z);
}
function percentile75(values: number[]): number {
  if (values.length === 0) return 0;
  values.sort((a, b) => a - b);
  return values[Math.min(values.length - 1, Math.floor((values.length - 1) * 0.75))];
}

/**
 * Estimate local surface complexity from already-accepted surfel normals.
 *
 * This is deliberately sign-invariant because the same physical surface can be
 * observed from camera poses that orient its normal differently. A singleton
 * orientation is never enough to declare an edge/corner. Smooth curvature is
 * captured by robust within-family angular spread; hard edges/corners are
 * captured by two or three independently supported orientation families.
 */
export function estimateSurfaceFeatureComplexity(
  evidence: Iterable<SurfaceNormalEvidence>,
): SurfaceFeatureComplexity {
  const normals: Array<{ normal: readonly [number, number, number]; confidence: number }> = [];
  for (const sample of evidence) {
    const confidence = Number(sample.normalConfidence);
    if (!Number.isFinite(confidence) || confidence < MIN_NORMAL_CONFIDENCE) continue;
    const normal = normalize(sample.normalX, sample.normalY, sample.normalZ);
    if (normal === null) continue;
    normals.push({ normal, confidence: clamp01(confidence) });
  }
  if (normals.length < 2) {
    return {
      score: 0,
      matureOrientationFamilies: 0,
      robustSpreadRadians: 0,
      strongestFamilySeparationRadians: 0,
      usableNormals: normals.length,
    };
  }

  const families: OrientationFamily[] = [];
  for (const sample of normals) {
    let best: OrientationFamily | null = null;
    let bestSimilarity = FAMILY_DOT;
    for (const family of families) {
      const reference = familyNormal(family);
      if (reference === null) continue;
      const similarity = absDot(reference, sample.normal);
      if (similarity >= bestSimilarity) { best = family; bestSimilarity = similarity; }
    }
    if (best === null) {
      families.push({
        x: sample.normal[0] * sample.confidence,
        y: sample.normal[1] * sample.confidence,
        z: sample.normal[2] * sample.confidence,
        weight: sample.confidence,
        members: 1,
      });
      continue;
    }
    const reference = familyNormal(best) ?? sample.normal;
    const aligned = signAligned(sample.normal, reference);
    best.x += aligned[0] * sample.confidence;
    best.y += aligned[1] * sample.confidence;
    best.z += aligned[2] * sample.confidence;
    best.weight += sample.confidence;
    best.members += 1;
  }

  const mature = families.filter((family) => family.members >= MIN_MATURE_FAMILY_MEMBERS);
  let strongestFamilySeparationRadians = 0;
  for (let first = 0; first < mature.length; first += 1) {
    const a = familyNormal(mature[first]);
    if (a === null) continue;
    for (let second = first + 1; second < mature.length; second += 1) {
      const b = familyNormal(mature[second]);
      if (b === null) continue;
      strongestFamilySeparationRadians = Math.max(
        strongestFamilySeparationRadians,
        Math.acos(absDot(a, b)),
      );
    }
  }

  // Measure robust curvature around the strongest family rather than max-angle,
  // so one flying normal cannot request a large memory budget by itself.
  const strongest = mature.length > 0
    ? mature.reduce((left, right) => right.weight > left.weight ? right : left)
    : families.reduce((left, right) => right.weight > left.weight ? right : left);
  const reference = familyNormal(strongest);
  const deviations = reference === null
    ? []
    : normals.map((sample) => Math.acos(absDot(reference, sample.normal)));
  const robustSpreadRadians = percentile75(deviations);

  const curvatureScore = clamp01(
    (robustSpreadRadians - CURVATURE_START_RADIANS) /
    (CURVATURE_FULL_RADIANS - CURVATURE_START_RADIANS),
  );
  const familyCountScore = mature.length <= 1 ? 0 : clamp01((mature.length - 1) / 2);
  const familySeparationScore = clamp01(strongestFamilySeparationRadians / FAMILY_SEPARATION_FULL_RADIANS);
  const hardFeatureScore = familyCountScore * (0.55 + familySeparationScore * 0.45);
  const score = clamp01(Math.max(curvatureScore * 0.78, hardFeatureScore));

  return {
    score,
    matureOrientationFamilies: mature.length,
    robustSpreadRadians,
    strongestFamilySeparationRadians,
    usableNormals: normals.length,
  };
}

/**
 * Keep flat cells cheap and spend local representatives where shape changes.
 * Global memory is still enforced by DepthPointCloudAccumulator, so this cannot
 * increase the scan-wide memory ceiling.
 */
export function featureAwareCellQuota(
  baseQuota: number,
  complexity: number,
  maximum = MAX_FEATURE_POINTS_PER_COVERAGE_CELL,
): number {
  const base = Math.max(1, Math.floor(baseQuota));
  const cap = Math.max(base, Math.floor(maximum));
  const bonus = Math.round(base * 0.72 * clamp01(complexity));
  return Math.min(cap, base + bonus);
}
