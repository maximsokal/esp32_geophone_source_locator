export interface SurfaceNormalEvidence {
  normalX: number;
  normalY: number;
  normalZ: number;
  normalConfidence: number;
  /** Optional measured position. When supplied, feature maturity also requires metric extent. */
  x?: number;
  y?: number;
  z?: number;
}

export interface SurfaceFeatureComplexity {
  score: number;
  matureOrientationFamilies: number;
  spatiallySupportedFamilies: number;
  robustSpreadRadians: number;
  strongestFamilySeparationRadians: number;
  usableNormals: number;
  spatialSpanMeters: number;
}

interface OrientationFamily {
  x: number;
  y: number;
  z: number;
  weight: number;
  members: number;
  positionedMembers: number;
  minX: number;
  minY: number;
  minZ: number;
  maxX: number;
  maxY: number;
  maxZ: number;
}

interface CanonicalSample {
  normal: readonly [number, number, number];
  confidence: number;
  position: readonly [number, number, number] | null;
}

const MIN_NORMAL_CONFIDENCE = 0.35;
const FAMILY_ANGLE_RADIANS = (24 * Math.PI) / 180;
const FAMILY_DOT = Math.cos(FAMILY_ANGLE_RADIANS);
const MIN_MATURE_FAMILY_MEMBERS = 2;
const MIN_MATURE_FAMILY_SPAN_METERS = 0.020;
const MIN_CURVATURE_SPAN_METERS = 0.035;
const FULL_SPATIAL_SUPPORT_METERS = 0.10;
const CURVATURE_START_RADIANS = (9 * Math.PI) / 180;
const CURVATURE_FULL_RADIANS = (34 * Math.PI) / 180;
const FAMILY_SEPARATION_FULL_RADIANS = (65 * Math.PI) / 180;
const CANONICAL_EPSILON = 1e-7;
export const MAX_FEATURE_POINTS_PER_COVERAGE_CELL = 48;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function normalize(x: number, y: number, z: number): readonly [number, number, number] | null {
  const length = Math.hypot(x, y, z);
  return Number.isFinite(length) && length > 1e-7 ? [x / length, y / length, z / length] : null;
}
function canonicalizeNormal(normal: readonly [number, number, number]): readonly [number, number, number] {
  const sign = Math.abs(normal[0]) > CANONICAL_EPSILON
    ? Math.sign(normal[0])
    : Math.abs(normal[1]) > CANONICAL_EPSILON
      ? Math.sign(normal[1])
      : Math.sign(normal[2]) || 1;
  return sign < 0 ? [-normal[0], -normal[1], -normal[2]] : normal;
}
function compareCanonicalSamples(a: CanonicalSample, b: CanonicalSample): number {
  if (a.confidence !== b.confidence) return b.confidence - a.confidence;
  if (a.normal[0] !== b.normal[0]) return a.normal[0] - b.normal[0];
  if (a.normal[1] !== b.normal[1]) return a.normal[1] - b.normal[1];
  if (a.normal[2] !== b.normal[2]) return a.normal[2] - b.normal[2];
  if (a.position === null) return b.position === null ? 0 : 1;
  if (b.position === null) return -1;
  if (a.position[0] !== b.position[0]) return a.position[0] - b.position[0];
  if (a.position[1] !== b.position[1]) return a.position[1] - b.position[1];
  return a.position[2] - b.position[2];
}
function absDot(a: readonly [number, number, number], b: readonly [number, number, number]): number {
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
function extent(
  minX: number, minY: number, minZ: number,
  maxX: number, maxY: number, maxZ: number,
): number {
  if (![minX, minY, minZ, maxX, maxY, maxZ].every(Number.isFinite)) return 0;
  return Math.hypot(maxX - minX, maxY - minY, maxZ - minZ);
}
function familySpatialSpan(family: OrientationFamily): number {
  return extent(family.minX, family.minY, family.minZ, family.maxX, family.maxY, family.maxZ);
}
function addPosition(family: OrientationFamily, position: readonly [number, number, number] | null): void {
  if (position === null) return;
  family.positionedMembers += 1;
  family.minX = Math.min(family.minX, position[0]);
  family.minY = Math.min(family.minY, position[1]);
  family.minZ = Math.min(family.minZ, position[2]);
  family.maxX = Math.max(family.maxX, position[0]);
  family.maxY = Math.max(family.maxY, position[1]);
  family.maxZ = Math.max(family.maxZ, position[2]);
}

/**
 * Estimate local surface complexity from already-accepted surfel normals.
 *
 * Angular evidence alone is insufficient when metric positions are available:
 * two conflicting normals packed into one tiny patch must not request the same
 * memory/probe budget as a measured bend, edge or corner that persists across a
 * meaningful surface extent. Each mature orientation family therefore needs
 * both repeated members and local metric span; smooth-curvature bonuses also
 * require spatial baseline. Callers without position data preserve the original
 * normal-only behaviour for compatibility/tests, but production surfels supply it.
 */
export function estimateSurfaceFeatureComplexity(
  evidence: Iterable<SurfaceNormalEvidence>,
): SurfaceFeatureComplexity {
  const normals: CanonicalSample[] = [];
  let positionedNormals = 0;
  let minX = Number.POSITIVE_INFINITY; let minY = Number.POSITIVE_INFINITY; let minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY; let maxY = Number.NEGATIVE_INFINITY; let maxZ = Number.NEGATIVE_INFINITY;
  for (const sample of evidence) {
    const confidence = Number(sample.normalConfidence);
    if (!Number.isFinite(confidence) || confidence < MIN_NORMAL_CONFIDENCE) continue;
    const normalized = normalize(sample.normalX, sample.normalY, sample.normalZ);
    if (normalized === null) continue;
    const hasPosition = Number.isFinite(sample.x) && Number.isFinite(sample.y) && Number.isFinite(sample.z);
    const position = hasPosition
      ? [Number(sample.x), Number(sample.y), Number(sample.z)] as const
      : null;
    if (position !== null) {
      positionedNormals += 1;
      minX = Math.min(minX, position[0]); minY = Math.min(minY, position[1]); minZ = Math.min(minZ, position[2]);
      maxX = Math.max(maxX, position[0]); maxY = Math.max(maxY, position[1]); maxZ = Math.max(maxZ, position[2]);
    }
    normals.push({ normal: canonicalizeNormal(normalized), confidence: clamp01(confidence), position });
  }
  normals.sort(compareCanonicalSamples);
  const spatialSpanMeters = positionedNormals >= 2 ? extent(minX, minY, minZ, maxX, maxY, maxZ) : 0;
  if (normals.length < 2) {
    return {
      score: 0,
      matureOrientationFamilies: 0,
      spatiallySupportedFamilies: 0,
      robustSpreadRadians: 0,
      strongestFamilySeparationRadians: 0,
      usableNormals: normals.length,
      spatialSpanMeters,
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
      const created: OrientationFamily = {
        x: sample.normal[0] * sample.confidence,
        y: sample.normal[1] * sample.confidence,
        z: sample.normal[2] * sample.confidence,
        weight: sample.confidence,
        members: 1,
        positionedMembers: 0,
        minX: Number.POSITIVE_INFINITY, minY: Number.POSITIVE_INFINITY, minZ: Number.POSITIVE_INFINITY,
        maxX: Number.NEGATIVE_INFINITY, maxY: Number.NEGATIVE_INFINITY, maxZ: Number.NEGATIVE_INFINITY,
      };
      addPosition(created, sample.position);
      families.push(created);
      continue;
    }
    const reference = familyNormal(best) ?? sample.normal;
    const aligned = signAligned(sample.normal, reference);
    best.x += aligned[0] * sample.confidence;
    best.y += aligned[1] * sample.confidence;
    best.z += aligned[2] * sample.confidence;
    best.weight += sample.confidence;
    best.members += 1;
    addPosition(best, sample.position);
  }

  const positionsAvailable = positionedNormals >= Math.max(2, Math.ceil(normals.length * 0.60));
  const mature = families.filter((family) => family.members >= MIN_MATURE_FAMILY_MEMBERS);
  const spatiallySupported = mature.filter((family) =>
    !positionsAvailable ||
    (family.positionedMembers >= MIN_MATURE_FAMILY_MEMBERS && familySpatialSpan(family) >= MIN_MATURE_FAMILY_SPAN_METERS)
  );
  let strongestFamilySeparationRadians = 0;
  for (let first = 0; first < spatiallySupported.length; first += 1) {
    const a = familyNormal(spatiallySupported[first]);
    if (a === null) continue;
    for (let second = first + 1; second < spatiallySupported.length; second += 1) {
      const b = familyNormal(spatiallySupported[second]);
      if (b === null) continue;
      strongestFamilySeparationRadians = Math.max(strongestFamilySeparationRadians, Math.acos(absDot(a, b)));
    }
  }

  const strongest = mature.length > 0
    ? mature.reduce((left, right) => right.weight > left.weight ? right : left)
    : families.reduce((left, right) => right.weight > left.weight ? right : left);
  const reference = familyNormal(strongest);
  const deviations = reference === null ? [] : normals.map((sample) => Math.acos(absDot(reference, sample.normal)));
  const robustSpreadRadians = percentile75(deviations);

  const curvatureAngularScore = clamp01(
    (robustSpreadRadians - CURVATURE_START_RADIANS) /
    (CURVATURE_FULL_RADIANS - CURVATURE_START_RADIANS),
  );
  const spatialScore = !positionsAvailable
    ? 1
    : clamp01((spatialSpanMeters - MIN_CURVATURE_SPAN_METERS) / (FULL_SPATIAL_SUPPORT_METERS - MIN_CURVATURE_SPAN_METERS));
  const curvatureScore = positionsAvailable && spatialSpanMeters < MIN_CURVATURE_SPAN_METERS
    ? 0
    : curvatureAngularScore * (0.45 + spatialScore * 0.55);
  const familyCount = spatiallySupported.length;
  const familyCountScore = familyCount <= 1 ? 0 : clamp01((familyCount - 1) / 2);
  const familySeparationScore = clamp01(strongestFamilySeparationRadians / FAMILY_SEPARATION_FULL_RADIANS);
  const hardFeatureScore = familyCountScore * (0.55 + familySeparationScore * 0.45) * (0.55 + spatialScore * 0.45);
  const score = clamp01(Math.max(curvatureScore * 0.78, hardFeatureScore));

  return {
    score,
    matureOrientationFamilies: mature.length,
    spatiallySupportedFamilies: spatiallySupported.length,
    robustSpreadRadians,
    strongestFamilySeparationRadians,
    usableNormals: normals.length,
    spatialSpanMeters,
  };
}

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
