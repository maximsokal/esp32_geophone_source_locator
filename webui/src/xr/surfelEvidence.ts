export type ProjectVector = readonly [number, number, number];
export type SampleUv = readonly [number, number];

export interface IndependentViewUpdate {
  independent: boolean;
  packed: number;
  count: number;
  code: number;
  minimumAngleRadians: number;
}

export interface DepthNeighbourhoodQuality {
  accepted: boolean;
  score: number;
  /** Depth DISCONTINUITY (silhouette/step): the sample may be a mixed pixel. */
  edge: boolean;
  dominantAxis: 'u' | 'v' | 'none';
  supportingNeighbours: number;
  /**
   * MAX-252: slope CHANGE with continuous depth (box edge, inside corner, fold).
   * A crease sample is a real surface point, but a symmetric normal stencil
   * straddles two faces there. Never set together with `edge`.
   */
  crease: boolean;
  creaseAxis: 'u' | 'v' | 'none';
  /** Signed distance (m) of the centre from the plane through the axis neighbours; > 0 = convex toward the camera. */
  creaseResidualMeters: number;
}

/**
 * Lateral position of each neighbour ray relative to the centre ray as a
 * view-space tangent (x/z), all magnitudes >= 0. 0 marks a neighbour that
 * reads the same depth pixel as the centre (unusable for curvature).
 */
export interface DepthStencilGeometry {
  left: number;
  right: number;
  up: number;
  down: number;
}

export interface SurfaceConfidenceInput {
  promoted: boolean;
  independentViews: number;
  weight: number;
  residualMeters: number;
  maxViewAngleRadians: number;
  contradictionWeight?: number;
  normalConfidence?: number;
}

const VIEW_AZIMUTH_BINS = 64;
const VIEW_ELEVATION_BINS = 32;
const VIEW_CODE_BASE = VIEW_AZIMUTH_BINS * VIEW_ELEVATION_BINS;
const MAX_PACKED_VIEW_CODES = 4;
const R2_PLASTIC = 1.3247179572447458;
const R2_ALPHA_1 = 1 / R2_PLASTIC;
const R2_ALPHA_2 = 1 / (R2_PLASTIC * R2_PLASTIC);

export const DEFAULT_INDEPENDENT_VIEW_ANGLE_RADIANS = (8 * Math.PI) / 180;
export const DEFAULT_SURFEL_NORMAL_GATE_RADIANS = (50 * Math.PI) / 180;

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

export function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

function fract(value: number): number {
  return value - Math.floor(value);
}

function hash32(value: number): number {
  let x = value | 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d);
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b);
  x ^= x >>> 16;
  return x >>> 0;
}

export function normalizeVector(vector: ProjectVector): ProjectVector {
  const length = Math.hypot(vector[0], vector[1], vector[2]);
  if (!Number.isFinite(length) || length < 1e-9) return [0, 0, 1];
  return [vector[0] / length, vector[1] / length, vector[2] / length];
}

export function dotVector(a: ProjectVector, b: ProjectVector): number {
  const na = normalizeVector(a);
  const nb = normalizeVector(b);
  return clamp(na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2], -1, 1);
}

export function angularDistance(a: ProjectVector, b: ProjectVector): number {
  return Math.acos(dotVector(a, b));
}

export function crossVector(a: ProjectVector, b: ProjectVector): ProjectVector {
  return normalizeVector([
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ]);
}

export function orientNormalToward(
  normal: ProjectVector,
  toward: ProjectVector,
): ProjectVector {
  const n = normalizeVector(normal);
  return dotVector(n, toward) < 0 ? [-n[0], -n[1], -n[2]] : n;
}

export function normalsCompatible(
  a: ProjectVector,
  b: ProjectVector,
  maxAngleRadians = DEFAULT_SURFEL_NORMAL_GATE_RADIANS,
): boolean {
  if (!Number.isFinite(maxAngleRadians) || maxAngleRadians <= 0) return false;
  return Math.acos(Math.abs(dotVector(a, b))) <= maxAngleRadians;
}

export function encodeViewDirection(direction: ProjectVector): number {
  const [x, y, z] = normalizeVector(direction);
  const azimuth01 = (Math.atan2(y, x) + Math.PI) / (Math.PI * 2);
  const elevation01 = (Math.asin(clamp(z, -1, 1)) + Math.PI / 2) / Math.PI;
  const azimuthBin = clamp(Math.floor(azimuth01 * VIEW_AZIMUTH_BINS), 0, VIEW_AZIMUTH_BINS - 1);
  const elevationBin = clamp(Math.floor(elevation01 * VIEW_ELEVATION_BINS), 0, VIEW_ELEVATION_BINS - 1);
  return elevationBin * VIEW_AZIMUTH_BINS + azimuthBin;
}

export function decodeViewDirection(code: number): ProjectVector {
  const safeCode = clamp(Math.floor(code), 0, VIEW_CODE_BASE - 1);
  const elevationBin = Math.floor(safeCode / VIEW_AZIMUTH_BINS);
  const azimuthBin = safeCode % VIEW_AZIMUTH_BINS;
  const azimuth = ((azimuthBin + 0.5) / VIEW_AZIMUTH_BINS) * Math.PI * 2 - Math.PI;
  const elevation = ((elevationBin + 0.5) / VIEW_ELEVATION_BINS) * Math.PI - Math.PI / 2;
  const cosElevation = Math.cos(elevation);
  return [cosElevation * Math.cos(azimuth), cosElevation * Math.sin(azimuth), Math.sin(elevation)];
}

export function packInitialView(direction: ProjectVector): { packed: number; count: number } {
  return { packed: encodeViewDirection(direction), count: 1 };
}

function packedViewCode(packed: number, index: number): number {
  return Math.floor(packed / (VIEW_CODE_BASE ** index)) % VIEW_CODE_BASE;
}

export function registerIndependentView(
  packed: number,
  count: number,
  candidateDirection: ProjectVector,
  minAngleRadians = DEFAULT_INDEPENDENT_VIEW_ANGLE_RADIANS,
): IndependentViewUpdate {
  const safeCount = clamp(Math.floor(count), 0, MAX_PACKED_VIEW_CODES);
  const code = encodeViewDirection(candidateDirection);
  const candidate = normalizeVector(candidateDirection);
  let minimumAngleRadians = Number.POSITIVE_INFINITY;
  for (let index = 0; index < safeCount; index += 1) {
    const existing = decodeViewDirection(packedViewCode(packed, index));
    minimumAngleRadians = Math.min(minimumAngleRadians, angularDistance(existing, candidate));
  }
  if (safeCount === 0) minimumAngleRadians = Math.PI;
  const independent = safeCount < MAX_PACKED_VIEW_CODES && minimumAngleRadians >= minAngleRadians;
  if (!independent) return { independent: false, packed, count: safeCount, code, minimumAngleRadians };
  return {
    independent: true,
    packed: packed + code * (VIEW_CODE_BASE ** safeCount),
    count: safeCount + 1,
    code,
    minimumAngleRadians,
  };
}

function median(values: number[]): number {
  values.sort((a, b) => a - b);
  const middle = Math.floor(values.length / 2);
  return values.length % 2 === 0
    ? (values[middle - 1] + values[middle]) * 0.5
    : values[middle];
}

/**
 * Depth sensor noise model used for crease decisions (1 sigma, metres):
 * ~1 mm floor plus the ARCore-like quadratic range term.
 */
export function depthNoiseSigmaMeters(depthMeters: number): number {
  return 0.001 + 0.0015 * depthMeters * depthMeters;
}

/*
 * The inverse-depth chord residual of a 3-sample stencil has noise ~sqrt(1.5)
 * sigma for a symmetric stencil; 3 of those keeps the per-axis false-positive
 * rate on a flat surface at ~0.3 %.
 */
const CREASE_NOISE_SIGMAS = 3;
const CREASE_RESIDUAL_NOISE_FACTOR = Math.sqrt(1.5);

export function creaseGateMeters(depthMeters: number): number {
  return CREASE_NOISE_SIGMAS * CREASE_RESIDUAL_NOISE_FACTOR * depthNoiseSigmaMeters(depthMeters);
}

/**
 * For ANY plane, inverse depth is affine in the view-space tangent, so the
 * centre's inverse depth lies on the chord through its two neighbours no
 * matter how steep the plane is or how unevenly the neighbours are spaced.
 * Returns the signed metric deviation from that plane (> 0: centre nearer).
 */
export function creaseResidualMeters(
  centerDepth: number,
  nearOffset: number,
  near: number | null,
  farOffset: number,
  far: number | null,
): number | null {
  if (near === null || far === null || !(nearOffset > 0) || !(farOffset > 0)) return null;
  if (!(centerDepth > 0) || !(near > 0) || !(far > 0)) return null;
  const chordInverse = (farOffset / near + nearOffset / far) / (nearOffset + farOffset);
  return 1 / chordInverse - centerDepth;
}

export function depthToleranceMeters(depthMeters: number): number {
  return clamp(0.020 + 0.0040 * depthMeters * depthMeters, 0.026, 0.105);
}

const NO_CREASE = { crease: false, creaseAxis: 'none', creaseResidualMeters: 0 } as const;

export function classifyDepthNeighbourhood(
  centerDepth: number,
  left: number | null,
  right: number | null,
  up: number | null,
  down: number | null,
  geometry: DepthStencilGeometry = { left: 1, right: 1, up: 1, down: 1 },
): DepthNeighbourhoodQuality {
  if (!Number.isFinite(centerDepth) || centerDepth <= 0) {
    return { accepted: false, score: 0, edge: false, dominantAxis: 'none', supportingNeighbours: 0, ...NO_CREASE };
  }

  const tolerance = depthToleranceMeters(centerDepth);
  const entries: Array<{ axis: 'u' | 'v'; value: number }> = [];
  const add = (axis: 'u' | 'v', value: number | null): void => {
    if (value !== null && Number.isFinite(value) && value > 0) entries.push({ axis, value });
  };
  add('u', left); add('u', right); add('v', up); add('v', down);

  if (entries.length < 2) {
    return { accepted: true, score: 0.34, edge: false, dominantAxis: 'none', supportingNeighbours: entries.length, ...NO_CREASE };
  }

  const residuals = entries.map((entry) => Math.abs(entry.value - centerDepth));
  const support = residuals.filter((residual) => residual <= tolerance);
  const discontinuityGate = Math.max(0.075, tolerance * 2.2);
  const far = entries.filter((entry) => Math.abs(entry.value - centerDepth) >= discontinuityGate);
  const edge = far.length > 0;

  let dominantAxis: 'u' | 'v' | 'none' = 'none';
  if (edge) {
    const uJump = Math.max(
      left === null ? 0 : Math.abs(left - centerDepth),
      right === null ? 0 : Math.abs(right - centerDepth),
    );
    const vJump = Math.max(
      up === null ? 0 : Math.abs(up - centerDepth),
      down === null ? 0 : Math.abs(down - centerDepth),
    );
    dominantAxis = uJump >= vJump ? 'u' : 'v';
  }

  // A discontinuity sample is a possible mixed pixel, never a crease.
  let creaseFields: Pick<DepthNeighbourhoodQuality, 'crease' | 'creaseAxis' | 'creaseResidualMeters'> = NO_CREASE;
  if (!edge) {
    const usable = (value: number | null): number | null => (value !== null && Number.isFinite(value) && value > 0 ? value : null);
    const uResidual = creaseResidualMeters(centerDepth, geometry.left, usable(left), geometry.right, usable(right));
    const vResidual = creaseResidualMeters(centerDepth, geometry.up, usable(up), geometry.down, usable(down));
    const uMagnitude = uResidual === null ? -1 : Math.abs(uResidual);
    const vMagnitude = vResidual === null ? -1 : Math.abs(vResidual);
    const axis: 'u' | 'v' = uMagnitude >= vMagnitude ? 'u' : 'v';
    const residual = axis === 'u' ? uResidual : vResidual;
    if (residual !== null && Math.abs(residual) >= creaseGateMeters(centerDepth)) {
      creaseFields = { crease: true, creaseAxis: axis, creaseResidualMeters: residual };
    }
  }

  if (support.length >= 2) {
    const inlierMedian = median(support.slice());
    const residualScore = 1 - clamp01(inlierMedian / tolerance);
    return {
      accepted: true,
      // A discontinuity sample can be a confident-but-wrong mixture of foreground
      // and background. Keep it as weak boundary evidence so sharp geometry is
      // not erased, but never give it near-interior surface weight.
      score: edge
        ? clamp01(0.22 + residualScore * 0.18)
        : clamp01(0.77 + residualScore * 0.23),
      edge,
      dominantAxis,
      supportingNeighbours: support.length,
      ...creaseFields,
    };
  }

  if (support.length === 1 && edge) {
    return { accepted: true, score: 0.18, edge: true, dominantAxis, supportingNeighbours: 1, ...NO_CREASE };
  }

  return { accepted: false, score: 0, edge, dominantAxis, supportingNeighbours: support.length, ...creaseFields };
}

/**
 * R2 low-discrepancy sample with a per-frame Cranley-Patterson rotation.
 * Unlike one-sample-per-grid-cell stratification, this has no permanent row /
 * column lattice for the accumulated map to reveal.
 */
export function lowDiscrepancySample2D(
  index: number,
  margin: number,
  phase: number,
): SampleUv {
  const safeIndex = Math.max(0, Math.floor(index));
  const safeMargin = clamp(margin, 0, 0.45);
  const usable = 1 - safeMargin * 2;
  const rotationU = (hash32(phase * 0x45d9f3b + 0x9e3779b9) + 0.5) / 4294967296;
  const rotationV = (hash32(phase * 0x27d4eb2d + 0x165667b1) + 0.5) / 4294967296;
  const sequenceIndex = safeIndex + 1;
  const u01 = fract(0.5 + R2_ALPHA_1 * sequenceIndex + rotationU);
  const v01 = fract(0.5 + R2_ALPHA_2 * sequenceIndex + rotationV);
  return [safeMargin + u01 * usable, safeMargin + v01 * usable];
}

export function surfaceRenderConfidence(input: SurfaceConfidenceInput): number {
  const viewScore = clamp01((input.independentViews - 1) / 2);
  const angleScore = clamp01(input.maxViewAngleRadians / ((16 * Math.PI) / 180));
  const residualScore = 1 - clamp01(input.residualMeters / 0.055);
  const weightScore = clamp01(input.weight / 4);
  const normalScore = clamp01(input.normalConfidence ?? 0.55);
  const contradictionPenalty = clamp01((input.contradictionWeight ?? 0) / 3);

  const raw = clamp01(
    0.05 +
    viewScore * 0.30 +
    angleScore * 0.20 +
    residualScore * 0.18 +
    weightScore * 0.10 +
    normalScore * 0.17 -
    contradictionPenalty * 0.38,
  );

  // A single-view / unpromoted hypothesis is intentionally never green.
  return input.promoted ? raw : Math.min(0.46, raw * 0.62);
}
